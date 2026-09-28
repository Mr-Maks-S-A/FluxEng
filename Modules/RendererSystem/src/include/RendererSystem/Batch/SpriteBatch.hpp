#pragma once
/**
 * @file SpriteBatch.hpp
 * @brief Сборка спрайтов кадра в вершины и минимальный набор draw call'ов (без OpenGL).
 */

#include <RendererSystem/Core/Color.hpp>
#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Handles.hpp>

#include <glm/vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace RendererSystem {

/// @brief Отражение спрайта (битовые флаги).
enum class SpriteFlip : std::uint8_t {
    None = 0,      ///< Без отражения.
    X = 1 << 0,    ///< По горизонтали.
    Y = 1 << 1,    ///< По вертикали.
    XY = X | Y,    ///< По обеим осям.
};

/// @brief Объединение флагов отражения.
[[nodiscard]] constexpr SpriteFlip operator|(SpriteFlip a, SpriteFlip b) noexcept {
    return static_cast<SpriteFlip>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// @brief `true`, если в `value` установлен флаг `flag`.
[[nodiscard]] constexpr bool has_flag(SpriteFlip value, SpriteFlip flag) noexcept {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
}

/**
 * @brief Один спрайт кадра — всё, что нужно, чтобы нарисовать прямоугольник с текстурой.
 *
 * Простая структура данных: её удобно заполнять из компонентов ECS в плотном цикле.
 */
struct SpriteInstance {
    glm::vec2 position{0.0f};            ///< Положение опорной точки в мире.
    glm::vec2 size{1.0f};                ///< Размер в единицах мира.
    glm::vec2 pivot{0.5f};               ///< Опорная точка внутри спрайта: (0,0) — левый верх, (1,1) — правый низ.
    float rotation = 0.0f;               ///< Поворот вокруг опорной точки, радианы (при оси Y вниз — по часовой).
    UvRect uv{};                         ///< Область текстуры (кадр спрайт-листа).
    Color color = Colors::white;         ///< Тонирование (умножается на цвет текстуры).
    TextureHandle texture = TextureHandle::white(); ///< Текстура.
    std::int32_t layer = 0;              ///< Слой: меньшие рисуются раньше (ниже).
    SpriteFlip flip = SpriteFlip::None;  ///< Отражение.
};

/**
 * @brief Вершина спрайта в том виде, в каком она уходит в GPU (20 байт).
 */
struct SpriteVertex {
    glm::vec2 position; ///< Позиция в мире.
    glm::vec2 uv;       ///< Текстурные координаты.
    Color color;        ///< Цвет RGBA8 (нормализуется в шейдере).
};

static_assert(sizeof(SpriteVertex) == 20, "SpriteVertex layout is part of the GPU contract");

/**
 * @brief Одна команда отрисовки: диапазон четырёхугольников с одной текстурой.
 */
struct DrawCommand {
    TextureHandle texture{};      ///< Текстура всей команды.
    std::uint32_t first_quad = 0; ///< Индекс первого четырёхугольника в SpriteBatch::vertices() / 4.
    std::uint32_t quad_count = 0; ///< Количество четырёхугольников.

    bool operator==(const DrawCommand&) const noexcept = default;
};

/**
 * @brief Порядок спрайтов внутри батча.
 *
 * Слои соблюдаются всегда. Разница — в порядке внутри одного слоя.
 */
enum class SortMode : std::uint8_t {
    /**
     * Внутри слоя спрайты группируются по текстуре — минимум draw call'ов.
     * Перекрывающиеся спрайты одного слоя с разными текстурами могут поменяться местами.
     * Подходит для тайлов, вокселей, частиц.
     */
    LayerThenTexture,
    /**
     * Внутри слоя сохраняется порядок отправки. Draw call'ов больше,
     * но порядок перекрытия точно такой, как у вызовов submit(). Подходит для UI.
     */
    LayerThenSubmission,
};

/**
 * @brief Собирает спрайты кадра в вершины и команды отрисовки.
 *
 * Жизненный цикл кадра: `clear()` → много `submit()` → `build()` → читать
 * vertices() и commands(). Класс не вызывает OpenGL, поэтому его можно
 * тестировать и профилировать без видеокарты, а в будущем — отдавать тот же
 * результат другому бэкенду.
 *
 * Память переиспользуется между кадрами: после прогрева аллокаций нет.
 */
class SpriteBatch {
public:
    /// @param mode Порядок спрайтов внутри слоя.
    explicit SpriteBatch(SortMode mode = SortMode::LayerThenTexture) noexcept : m_mode(mode) {}

    /// @brief Порядок спрайтов.
    [[nodiscard]] SortMode sort_mode() const noexcept { return m_mode; }
    /// @brief Меняет порядок спрайтов для следующих build().
    void set_sort_mode(SortMode mode) noexcept { m_mode = mode; }

    /// @brief Резервирует место под `sprites` спрайтов.
    void reserve(std::size_t sprites);
    /// @brief Удаляет отправленные спрайты и результаты сборки (память сохраняется).
    void clear() noexcept;

    /// @brief Добавляет спрайт.
    void submit(const SpriteInstance& sprite) { m_sprites.push_back(sprite); }

    /// @brief Залитый прямоугольник (через белую текстуру).
    void submit_rect(const Rect& rect, Color color, std::int32_t layer = 0);
    /// @brief Отрезок толщиной `thickness` (прямоугольник, повёрнутый вдоль отрезка).
    void submit_line(glm::vec2 from, glm::vec2 to, float thickness, Color color, std::int32_t layer = 0);
    /// @brief Контур прямоугольника толщиной `thickness`, рисуется внутрь.
    void submit_rect_outline(const Rect& rect, float thickness, Color color, std::int32_t layer = 0);

    /**
     * @brief Сортирует спрайты и строит вершины и команды.
     *
     * Если спрайты уже идут в нужном порядке (частый случай: один слой, одна текстура),
     * сортировка пропускается.
     */
    void build();

    /// @brief Отправленные спрайты (в порядке submit()).
    [[nodiscard]] std::span<const SpriteInstance> sprites() const noexcept { return m_sprites; }
    /// @brief Вершины после build(): по 4 на спрайт (левый верх, правый верх, правый низ, левый низ).
    [[nodiscard]] std::span<const SpriteVertex> vertices() const noexcept { return m_vertices; }
    /// @brief Команды после build().
    [[nodiscard]] std::span<const DrawCommand> commands() const noexcept { return m_commands; }
    /// @brief Количество четырёхугольников после build().
    [[nodiscard]] std::size_t quad_count() const noexcept { return m_vertices.size() / 4; }

    /**
     * @brief Записывает 4 вершины спрайта в `out`.
     *
     * Открыто для тестов и для тех, кому нужны вершины без батча.
     */
    static void write_quad(const SpriteInstance& sprite, SpriteVertex* out) noexcept;

private:
    [[nodiscard]] std::uint64_t sort_key(const SpriteInstance& sprite) const noexcept;
    void sort_order();

    SortMode m_mode;
    std::vector<SpriteInstance> m_sprites;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> m_order; // (ключ, индекс спрайта)
    std::vector<std::pair<std::uint64_t, std::uint32_t>> m_scratch; // буфер поразрядной сортировки
    std::vector<SpriteVertex> m_vertices;
    std::vector<DrawCommand> m_commands;
};

} // namespace RendererSystem
