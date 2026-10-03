#pragma once
/**
 * @file Renderer2D.hpp
 * @brief Батчевый 2D-рендер поверх RHI (OpenGL или Vulkan): спрайты, прямоугольники, линии, текст.
 */

#include <RendererSystem/Batch/SpriteBatch.hpp>
#include <RendererSystem/Core/Color.hpp>
#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Handles.hpp>
#include <RendererSystem/Core/Image.hpp>
#include <RendererSystem/RHI/Device.hpp>
#include <RendererSystem/RHI/Resources.hpp>
#include <RendererSystem/Scene/Camera2D.hpp>
#include <RendererSystem/Text/Font.hpp>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace RendererSystem {

/**
 * @brief Настройки Renderer2D.
 */
struct RendererConfig {
    std::size_t initial_quad_capacity = 4096;     ///< Начальная ёмкость GPU-буферов; растёт автоматически.
    SortMode sort_mode = SortMode::LayerThenTexture; ///< Порядок спрайтов внутри слоя.
};

/**
 * @brief Статистика последнего кадра.
 */
struct RenderStats {
    std::uint32_t quads = 0;         ///< Нарисовано четырёхугольников.
    std::uint32_t draw_calls = 0;    ///< Вызовов glDrawElements.
    std::uint32_t texture_binds = 0; ///< Смен текстуры.
};

/**
 * @brief Как рисовать текст.
 */
struct TextStyle {
    float size = 0.0f;                    ///< Кегль в единицах мира/экрана; 0 — как запечён шрифт.
    Color color = Colors::white;          ///< Цвет.
    TextAlign align = TextAlign::Left;    ///< Выравнивание строк.
    float max_width = 0.0f;               ///< Перенос по словам; 0 — без переноса.
    float line_spacing = 1.0f;            ///< Множитель межстрочного интервала.
    std::int32_t layer = 0;               ///< Слой спрайтов текста.
    Color shadow = Colors::transparent;   ///< Цвет тени (прозрачный — без тени).
    glm::vec2 shadow_offset{1.5f, 1.5f};  ///< Смещение тени.
};

/**
 * @brief 2D-рендер: собирает кадр в SpriteBatch и рисует его минимумом draw call'ов.
 *
 * Рендеру нужно только устройство RHI (OpenGL или Vulkan) — окно, ввод и время кадра
 * принадлежат другим модулям. Рисует в текущую цель устройства (экран или RenderTarget).
 *
 * Кадр:
 * @code
 * renderer.set_viewport(window.getWidth(), window.getHeight());
 * renderer.clear(Colors::black);
 * renderer.begin(camera);
 * renderer.draw(SpriteInstance{.position = {10, 20}, .size = {16, 16}, .texture = goblin});
 * renderer.fill_rect({{0, 0}, {100, 5}}, Colors::red);
 * RenderStats stats = renderer.end();
 * @endcode
 *
 * @note Не потокобезопасен (как и устройство). Должен умереть раньше устройства.
 */
class Renderer2D {
public:
    /**
     * @brief Создаёт рендер на устройстве.
     * @return Рендер или текст ошибки (не собрался встроенный шейдер).
     */
    [[nodiscard]] static std::expected<Renderer2D, std::string> create(RHI::Device& device, const RendererConfig& config = {});

    ~Renderer2D();
    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;
    Renderer2D(Renderer2D&& other) noexcept;
    Renderer2D& operator=(Renderer2D&& other) noexcept;

    // ================================================================= ресурсы

    /// @brief Загружает изображение в видеопамять и возвращает дескриптор.
    [[nodiscard]] TextureHandle create_texture(const Image& image, const TextureDesc& desc = {});

    /// @brief Загружает изображение из файла.
    [[nodiscard]] std::expected<TextureHandle, std::string> load_texture(const std::filesystem::path& path,
                                                                         const TextureDesc& desc = {});

    /**
     * @brief Заменяет пиксели существующей текстуры (размер тот же): живые карты, мини-карта, тепловые слои.
     * @throws RendererError Дескриптор не из этого рендера или размер отличается.
     */
    void update_texture(TextureHandle handle, const Image& image);

    /**
     * @brief Текстура по дескриптору.
     * @throws RendererError Дескриптор не из этого рендера.
     */
    [[nodiscard]] const Texture& texture(TextureHandle handle) const;

    /// @brief Количество текстур (включая встроенную белую).
    [[nodiscard]] std::size_t texture_count() const noexcept { return m_textures.size(); }

    /**
     * @brief Забирает шрифт и загружает его атлас в видеопамять.
     *
     * По умолчанию атлас фильтруется линейно: текст можно рисовать в любом кегле.
     */
    [[nodiscard]] FontHandle add_font(Font font, const TextureDesc& desc = {.filter = TextureFilter::Linear});

    /**
     * @brief Шрифт по дескриптору.
     * @throws RendererError Дескриптор не из этого рендера.
     */
    [[nodiscard]] const Font& font(FontHandle handle) const;

    /// @brief Размер блока текста при стиле `style`.
    [[nodiscard]] glm::vec2 measure_text(FontHandle handle, std::string_view text, const TextStyle& style = {}) const;

    // ================================================================= кадр

    /// @brief Область вывода в текущей цели устройства.
    void set_viewport(int width, int height);

    /// @brief Очищает текущую цель цветом.
    void clear(Color color);

    /// @brief Устройство, на котором работает рендер.
    [[nodiscard]] RHI::Device& device() const noexcept { return *m_device; }

    /// @brief Начинает кадр с матрицей `view_projection`.
    void begin(const glm::mat4& view_projection);
    /// @brief Начинает кадр с камерой.
    void begin(const Camera2D& camera) { begin(camera.view_projection()); }

    /// @brief Спрайт.
    void draw(const SpriteInstance& sprite) { m_batch.submit(sprite); }
    /// @brief Залитый прямоугольник.
    void fill_rect(const Rect& rect, Color color, std::int32_t layer = 0) { m_batch.submit_rect(rect, color, layer); }
    /// @brief Контур прямоугольника.
    void draw_rect(const Rect& rect, float thickness, Color color, std::int32_t layer = 0) {
        m_batch.submit_rect_outline(rect, thickness, color, layer);
    }
    /// @brief Отрезок.
    void draw_line(glm::vec2 from, glm::vec2 to, float thickness, Color color, std::int32_t layer = 0) {
        m_batch.submit_line(from, to, thickness, color, layer);
    }

    /**
     * @brief Текст UTF-8; `top_left` — левый верх блока (при выравнивании по центру/вправо блок шириной max_width).
     * @return Размер нарисованного блока.
     */
    glm::vec2 draw_text(FontHandle handle, std::string_view text, glm::vec2 top_left, const TextStyle& style = {});

    /**
     * @brief Завершает кадр: сортирует, загружает вершины и рисует.
     * @return Статистика кадра.
     */
    RenderStats end();

    /// @brief Батч текущего кадра (например чтобы заполнить его напрямую из ECS).
    [[nodiscard]] SpriteBatch& batch() noexcept { return m_batch; }

    /// @brief Статистика последнего end().
    [[nodiscard]] const RenderStats& last_stats() const noexcept { return m_stats; }

private:
    Renderer2D(RHI::Device& device, Pipeline pipeline, const RendererConfig& config);

    void ensure_capacity(std::size_t quads);
    void release() noexcept;

    struct FontEntry {
        Font font;
        TextureHandle texture;
    };

    RHI::Device* m_device = nullptr;
    Pipeline m_pipeline;
    std::vector<Texture> m_textures;
    std::vector<FontEntry> m_fonts;
    std::vector<GlyphQuad> m_glyphs; ///< Раскладка текущего draw_text (память переиспользуется).
    SpriteBatch m_batch;
    glm::mat4 m_view_projection{1.0f};
    RenderStats m_stats{};

    RHI::BufferId m_vertices{}; ///< Stream: вершины кадра.
    RHI::BufferId m_indices{};  ///< Static: шесть индексов на четырёхугольник, растёт по требованию.
    std::size_t m_quad_capacity = 0;
    bool m_in_frame = false;
};

} // namespace RendererSystem
