#pragma once
/**
 * @file Types.hpp
 * @brief Типы слоя RHI (Render Hardware Interface): описания ресурсов, конвейеров и вызовов рисования.
 *
 * RHI — тонкая граница между рендерами модуля (Renderer2D, Renderer3D, свои конвейеры игры) и графическим
 * API. Выше неё нет ни одного вызова OpenGL или Vulkan; ниже — бэкенды (RHI::Device).
 *
 * Ресурсы выдаются как **ручки** (индексы, нулевая — «нет», ZII): их можно хранить в компонентах ECS,
 * копировать и сравнивать. Владение — через RAII-обёртки в Resources.hpp.
 */

#include <RendererSystem/Core/Color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace RendererSystem {

/// @brief Графический API бэкенда.
enum class Backend : std::uint8_t {
    OpenGL, ///< OpenGL 3.3 core (glad).
    Vulkan, ///< Vulkan 1.3 (dynamic rendering), шейдеры компилирует shaderc.
};

/// @brief Имя бэкенда: "opengl" / "vulkan".
[[nodiscard]] std::string_view to_string(Backend backend) noexcept;
/// @brief "gl", "opengl", "vk", "vulkan" → бэкенд.
[[nodiscard]] std::optional<Backend> parse_backend(std::string_view name) noexcept;
/// @brief Собран ли бэкенд в эту библиотеку (Vulkan — если при сборке нашлись Vulkan SDK и shaderc).
[[nodiscard]] bool backend_compiled(Backend backend) noexcept;
/// @brief Все собранные бэкенды.
[[nodiscard]] std::vector<Backend> compiled_backends();

/// @brief Фильтрация текстуры.
enum class TextureFilter : std::uint8_t {
    Nearest, ///< Без сглаживания — чёткие пиксели (пиксель-арт, воксельные атласы).
    Linear,  ///< Билинейное сглаживание.
};

/// @brief Поведение UV за пределами 0..1.
enum class TextureWrap : std::uint8_t {
    ClampToEdge, ///< Край растягивается (нет «швов» у атласов).
    Repeat,      ///< Текстура повторяется.
};

/// @brief Параметры текстуры (RGBA8).
struct TextureDesc {
    TextureFilter filter = TextureFilter::Nearest; ///< Фильтрация (по умолчанию — для пиксель-арта).
    TextureWrap wrap = TextureWrap::ClampToEdge;   ///< Повторение.
    bool mipmaps = false;                          ///< Строить mip-уровни.
};

/// @brief Параметры цели рендера (рендер в текстуру).
struct TargetDesc {
    TextureDesc color{};  ///< Цветовая текстура.
    bool depth = false;   ///< Буфер глубины (нужен для 3D).
};

namespace RHI {

/// @brief Ручка буфера вершин или индексов (0 — нет).
struct BufferId {
    std::uint32_t index = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    friend constexpr bool operator==(BufferId, BufferId) noexcept = default;
};
/// @brief Ручка текстуры (0 — нет: рисуется белой).
struct TextureId {
    std::uint32_t index = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    friend constexpr bool operator==(TextureId, TextureId) noexcept = default;
};
/// @brief Ручка цели рендера (0 — экран).
struct TargetId {
    std::uint32_t index = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    friend constexpr bool operator==(TargetId, TargetId) noexcept = default;
};
/// @brief Ручка конвейера (шейдеры + состояние).
struct PipelineId {
    std::uint32_t index = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    friend constexpr bool operator==(PipelineId, PipelineId) noexcept = default;
};

/// @brief Что хранит буфер.
enum class BufferKind : std::uint8_t { Vertex, Index };

/// @brief Как часто меняется содержимое (подсказка бэкенду).
enum class BufferUsage : std::uint8_t {
    Static,  ///< Загружается один раз (местность, карты, модели).
    Dynamic, ///< Иногда меняется (сетка чанка после правки).
    Stream,  ///< Каждый кадр (батч спрайтов, частицы). В Vulkan живёт в кольце кадра.
};

/// @brief Тип компоненты атрибута вершины.
enum class AttributeType : std::uint8_t {
    Float,            ///< `float`.
    UnsignedByteNorm, ///< `uint8`, нормализуется в 0…1 (цвет RGBA8).
};

/// @brief Один атрибут вершины: `FLUX_LOCATION(N) in ...` в шейдере.
struct VertexAttribute {
    std::uint32_t location = 0;                ///< Номер атрибута в шейдере.
    std::uint32_t components = 0;              ///< 1…4.
    AttributeType type = AttributeType::Float; ///< Тип компоненты.
    std::size_t offset = 0;                    ///< Смещение от начала вершины, байты.
};

/**
 * @brief Как читать вершину из буфера: шаг и атрибуты (до 8).
 * @code
 * struct VoxelVertex { float x, y, z; std::uint32_t rgba; };
 * constexpr RHI::VertexLayout layout = RHI::VertexLayout::make(sizeof(VoxelVertex), {
 *     {0, 3, RHI::AttributeType::Float, 0},
 *     {1, 4, RHI::AttributeType::UnsignedByteNorm, offsetof(VoxelVertex, rgba)}});
 * @endcode
 */
struct VertexLayout {
    static constexpr std::size_t max_attributes = 8; ///< Предел атрибутов.

    std::size_t stride = 0;                                   ///< Размер вершины, байты.
    std::array<VertexAttribute, max_attributes> attributes{}; ///< Атрибуты.
    std::size_t attribute_count = 0;                          ///< Сколько задано.

    /// @brief Раскладка из списка атрибутов.
    [[nodiscard]] static constexpr VertexLayout make(std::size_t stride, std::initializer_list<VertexAttribute> list) {
        VertexLayout layout{.stride = stride};
        for (const VertexAttribute& attribute : list) {
            if (layout.attribute_count < max_attributes) layout.attributes[layout.attribute_count++] = attribute;
        }
        return layout;
    }
    /// @brief Заданные атрибуты.
    [[nodiscard]] constexpr std::span<const VertexAttribute> used() const noexcept { return {attributes.data(), attribute_count}; }
};

/// @brief Примитивы.
enum class Primitive : std::uint8_t { Triangles, Lines };

/// @brief Смешивание с тем, что уже нарисовано.
enum class BlendMode : std::uint8_t {
    Opaque,   ///< Непрозрачно.
    Alpha,    ///< Прозрачность по альфе.
    Additive, ///< Сложение цветов: свечение, искры, магия.
};

/// @brief Отсечение граней.
enum class CullMode : std::uint8_t { None, Back };

/// @brief Работа с буфером глубины.
enum class DepthMode : std::uint8_t {
    Off,       ///< Без глубины (2D, оверлей).
    Test,      ///< Проверять, но не писать (прозрачное).
    TestWrite, ///< Проверять и писать (непрозрачное 3D).
};

/**
 * @brief Исходники шейдеров — один GLSL на оба бэкенда.
 *
 * Строку `#version` и макросы добавляет бэкенд. В тексте используйте:
 * - `FLUX_LOCATION(n)` — вход вершинного шейдера и выход фрагментного (`layout(location = n)`);
 * - `FLUX_VARYING(n)` — передача между шейдерами (номер нужен Vulkan, в GL 3.3 — пусто);
 * - `FLUX_UNIFORM(set, binding) Frame { ... } frame;` — блок кадра (set 0), `Draw` — блок вызова (set 1);
 * - `FLUX_SAMPLER(2, 0) sampler2D u_texture;` — текстура вызова;
 * - `FLUX_POSITION(clip);` — запись gl_Position (Vulkan переводит глубину −1…1 в 0…1).
 * Макрос `FLUX_VULKAN` или `FLUX_OPENGL` определён — можно ветвиться.
 */
struct ShaderSource {
    std::string_view vertex;   ///< Вершинный шейдер (тело без #version).
    std::string_view fragment; ///< Фрагментный шейдер.
};

/// @brief Предел блока кадра (set 0) и блока вызова (set 1), байты.
inline constexpr std::size_t max_frame_uniforms = 1024;
inline constexpr std::size_t max_draw_uniforms = 256;

/// @brief Конвейер: шейдеры, раскладка вершины и неизменяемое состояние.
struct PipelineDesc {
    std::string name;                         ///< Для сообщений об ошибках.
    ShaderSource shader{};                    ///< GLSL.
    VertexLayout layout{};                    ///< Вершина.
    Primitive primitive = Primitive::Triangles;
    BlendMode blend = BlendMode::Opaque;
    CullMode cull = CullMode::None;
    DepthMode depth = DepthMode::Off;
};

/// @brief Данные блока uniform, уже лежащие в памяти кадра (результат Device::push_uniforms). Нулевой — «нет».
struct UniformSlice {
    std::uint32_t chunk = 0;  ///< Внутренний номер буфера кольца + 1.
    std::uint32_t offset = 0; ///< Смещение в буфере.
    std::uint32_t size = 0;   ///< Размер.
    [[nodiscard]] constexpr bool valid() const noexcept { return chunk != 0; }
};

/// @brief Один вызов рисования.
struct DrawCall {
    PipelineId pipeline{};                     ///< Конвейер.
    BufferId vertices{};                       ///< Вершины.
    BufferId indices{};                        ///< Индексы (uint32) или нет.
    std::uint32_t first = 0;                   ///< Первый индекс (или вершина без индексов).
    std::uint32_t count = 0;                   ///< Сколько индексов (вершин).
    TextureId texture{};                       ///< Текстура (нет — белая 1×1).
    UniformSlice frame{};                      ///< Блок кадра (set 0).
    std::span<const std::byte> draw_uniforms{}; ///< Блок вызова (set 1), копируется сразу.
};

/// @brief Счётчики кадра (сбрасываются в begin_frame).
struct DeviceStats {
    std::uint32_t draw_calls = 0;      ///< Вызовов рисования.
    std::uint32_t pipeline_binds = 0;  ///< Смен конвейера.
    std::uint32_t passes = 0;          ///< Проходов (смен цели или очисток).
    std::uint64_t uploaded_bytes = 0;  ///< Загружено в видеопамять (буферы, uniform, текстуры).
};

/// @brief Что за устройство.
struct DeviceInfo {
    Backend backend = Backend::OpenGL;
    std::string device_name; ///< Видеокарта / рендерер драйвера.
    std::string api_version; ///< Версия API.
    bool presents = false;   ///< Есть экран (окно); иначе только рендер в текстуры.
};

/// @brief Параметры создания устройства.
struct DeviceConfig {
    Backend backend = Backend::OpenGL;
    /// Vulkan: слои валидации (сообщения — в stderr и Device::validation_messages()).
    bool validation = false;
    /// Vulkan: показ с ожиданием кадра монитора (FIFO) или без (MAILBOX/IMMEDIATE).
    bool vsync = true;
    /// Vulkan: расширения экземпляра для окна (Window::vulkan_instance_extensions()). Пусто — без окна.
    std::vector<std::string> instance_extensions{};
    /// Vulkan: создать VkSurfaceKHR по VkInstance (Window::create_vulkan_surface). Пусто — без окна.
    std::function<std::expected<std::uint64_t, std::string>(std::uintptr_t instance)> create_surface{};
};

} // namespace RHI
} // namespace RendererSystem
