#pragma once
/**
 * @file Renderer3D.hpp
 * @brief 3D-рендер поверх RHI (OpenGL или Vulkan): сетки с материалами, освещение, туман, прозрачность, отсечение.
 */

#include <RendererSystem/Core/Color.hpp>
#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/RHI/Device.hpp>
#include <RendererSystem/RHI/Resources.hpp>
#include <RendererSystem/Scene/Camera3D.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace RendererSystem {

/// @brief Направленный свет («солнце»): лучи параллельны `direction`.
struct DirectionalLight {
    glm::vec3 direction{-0.35f, -1.0f, -0.45f}; ///< Куда светит (не обязательно единичный).
    Color color = Colors::white;                ///< Цвет.
    float intensity = 1.0f;                     ///< Множитель яркости.
};

/// @brief Точечный свет: факел, снаряд, вспышка. Затухает до нуля на расстоянии `radius`.
struct PointLight {
    glm::vec3 position{0.0f}; ///< Положение.
    Color color = Colors::white; ///< Цвет.
    float intensity = 1.0f;   ///< Множитель яркости.
    float radius = 5.0f;      ///< Дальность.
};

/**
 * @brief Освещение и атмосфера кадра.
 */
struct Environment {
    static constexpr std::size_t max_point_lights = 8; ///< Предел точечных источников в кадре.

    Color ambient{64, 66, 78, 255};                      ///< Рассеянный свет (тени не бывают чёрными).
    DirectionalLight sun{};                              ///< Основной свет.
    std::array<PointLight, max_point_lights> points{};   ///< Точечные источники.
    std::size_t point_count = 0;                         ///< Сколько из них задано.
    Color fog_color = Colors::black;                     ///< Цвет тумана.
    float fog_start = 0.0f;                              ///< Начало тумана (расстояние от камеры).
    float fog_end = 0.0f;                                ///< Полный туман; `fog_end <= fog_start` — без тумана.

    /// @brief Добавляет точечный свет; `false`, если места нет (лишние источники отбрасываются).
    bool add_point(const PointLight& light) noexcept {
        if (point_count >= max_point_lights) return false;
        points[point_count++] = light;
        return true;
    }
};

/// @brief Смешивание: Opaque пишет глубину и рисуется первым (группами по текстуре);
/// Alpha и Additive — после непрозрачного, от дальних к ближним, без записи глубины.
using BlendMode = RHI::BlendMode;

/**
 * @brief Как выглядит поверхность.
 *
 * Итоговый цвет = текстура × `color` × цвет вершины × освещение + `emissive`.
 */
struct Material {
    Color color = Colors::white;            ///< Тонирование.
    const Texture* texture = nullptr;      ///< Текстура (не владеет); nullptr — белая.
    UvRect uv{};                            ///< Область текстуры: UV сетки 0…1 растягиваются на неё (атлас, RenderTarget::uv()).
    glm::vec3 emissive{0.0f};               ///< Собственное свечение (не зависит от света).
    float specular = 0.25f;                 ///< Сила бликов.
    float shininess = 24.0f;                ///< Резкость бликов.
    bool lit = true;                        ///< `false` — без освещения (интерфейс в мире, небо, эффекты).
    BlendMode blend = BlendMode::Opaque;    ///< Смешивание.
    bool double_sided = false;              ///< Рисовать обе стороны треугольников.
};

/// @brief Статистика последнего кадра Renderer3D.
struct Render3DStats {
    std::uint32_t draws = 0;          ///< Нарисовано объектов.
    std::uint32_t culled = 0;         ///< Отброшено пирамидой видимости.
    std::uint32_t triangles = 0;      ///< Треугольников отправлено.
    std::uint32_t texture_binds = 0;  ///< Смен текстуры.
    std::uint32_t transparent = 0;    ///< Из них прозрачных.
};

/**
 * @brief 3D-рендер: собирает команды кадра, сортирует и рисует одним шейдером с освещением.
 *
 * Кадр:
 * @code
 * renderer3d.clear(sky);                                   // цвет + глубина
 * renderer3d.begin(camera, environment);
 * renderer3d.draw(table_mesh, glm::mat4{1.0f}, {.texture = &wood});
 * renderer3d.draw_shape(Renderer3D::Shape::Sphere, model, {.color = gold, .emissive = {1, 0.8f, 0.2f}});
 * Render3DStats stats = renderer3d.end();                  // GL-состояние возвращается для 2D-оверлея
 * @endcode
 *
 * Сетка с известными границами (Mesh::bounds()) отсекается по пирамиде видимости камеры.
 *
 * @note Не потокобезопасен. Должен умереть раньше устройства.
 */
class Renderer3D {
public:
    /// @brief Встроенные сетки единичного размера (центр в начале координат).
    enum class Shape : std::uint8_t {
        Cube,     ///< Куб 1×1×1.
        Sphere,   ///< Сфера диаметром 1.
        Quad,     ///< Квадрат 1×1 в плоскости XY лицом к +Z.
        Plane,    ///< Квадрат 1×1 в плоскости XZ лицом вверх.
        Cylinder, ///< Цилиндр диаметром 1 и высотой 1 вдоль Y.
        Count,
    };

    /**
     * @brief Создаёт рендер на устройстве.
     * @return Рендер или текст ошибки (не собрался шейдер).
     */
    [[nodiscard]] static std::expected<Renderer3D, std::string> create(RHI::Device& device);

    Renderer3D(Renderer3D&&) noexcept = default;
    Renderer3D& operator=(Renderer3D&&) noexcept = default;
    Renderer3D(const Renderer3D&) = delete;
    Renderer3D& operator=(const Renderer3D&) = delete;
    ~Renderer3D() = default;

    /// @brief Очищает текущую цель устройства цветом и буфер глубины.
    void clear(Color color);

    /// @brief Начинает кадр.
    void begin(const Camera3D& camera, const Environment& environment = {});

    /// @brief Добавляет сетку с матрицей модели и материалом.
    void draw(const Mesh& mesh, const glm::mat4& model, const Material& material = {});
    /// @brief Добавляет встроенную фигуру.
    void draw_shape(Shape shape, const glm::mat4& model, const Material& material = {}) {
        draw(this->shape(shape), model, material);
    }

    /**
     * @brief Сортирует и рисует кадр.
     *
     * Состояние (глубина, смешивание, отсечение) задано конвейерами — после вызова можно сразу рисовать 2D.
     * @return Статистика кадра.
     */
    Render3DStats end();

    /// @brief Встроенная сетка.
    [[nodiscard]] const Mesh& shape(Shape shape) const noexcept { return m_shapes[static_cast<std::size_t>(shape)]; }
    /// @brief Устройство.
    [[nodiscard]] RHI::Device& device() const noexcept { return *m_device; }
    /// @brief Камера текущего (или последнего) кадра.
    [[nodiscard]] const Camera3D& camera() const noexcept { return m_camera; }
    /// @brief Статистика последнего end().
    [[nodiscard]] const Render3DStats& last_stats() const noexcept { return m_stats; }

private:
    struct Command {
        const Mesh* mesh = nullptr;
        glm::mat4 model{1.0f};
        Material material{};
        float distance = 0.0f; ///< До камеры — для сортировки прозрачных.
    };

    explicit Renderer3D(RHI::Device& device);
    void submit(const Command& command, RHI::UniformSlice frame);
    [[nodiscard]] RHI::PipelineId pipeline_for(const Material& material) const noexcept;

    RHI::Device* m_device = nullptr;
    std::array<Pipeline, 6> m_pipelines{}; ///< [смешивание × 2 + (двусторонний ? 0 : 1)]
    std::array<Mesh, static_cast<std::size_t>(Shape::Count)> m_shapes{};
    std::vector<Command> m_opaque;
    std::vector<Command> m_transparent;
    Camera3D m_camera{};
    Frustum m_frustum{};
    Environment m_environment{};
    Render3DStats m_stats{};
    bool m_in_frame = false;
};

} // namespace RendererSystem
