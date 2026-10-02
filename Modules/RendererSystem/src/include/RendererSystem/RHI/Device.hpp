#pragma once
/**
 * @file Device.hpp
 * @brief RHI::Device — графическое устройство: ресурсы, кадр, вызовы рисования. Бэкенды: OpenGL и Vulkan.
 *
 * Модель — «машина состояний», как привычный OpenGL, но без глобального состояния драйвера:
 *
 * @code
 * auto device = RHI::Device::create({.backend = Backend::Vulkan, ...}).value();
 * device->begin_frame(width, height);           // Vulkan: взять изображение swapchain'а
 * device->bind_target({});                      // экран (или ручка цели — рендер в текстуру)
 * device->clear(Colors::black, true);           // цвет + глубина
 * const RHI::UniformSlice frame = device->push_uniform(view_projection);
 * device->draw({.pipeline = p, .vertices = vb, .indices = ib, .count = 36, .frame = frame,
 *               .draw_uniforms = as_bytes(model)});
 * device->end_frame();                          // Vulkan: отправить и показать; GL: ничего (окно меняет буферы)
 * @endcode
 *
 * Правила, общие для бэкендов (их проверяют тесты на обоих):
 * - строка 0 изображений — верх (Image), UV (0,0) — левый верх; целевую текстуру рисуйте с target_uv();
 * - глубина проекции — как в OpenGL (−1…1, glm по умолчанию): Vulkan-бэкенд переводит её сам (FLUX_POSITION);
 * - обход против часовой стрелки — лицевая сторона;
 * - всё, что выделено push_uniform / Stream-буферами, живёт до конца кадра.
 *
 * Ошибки данных (шейдер не собрался, нет устройства) — `std::expected`; ошибки использования (чужая ручка,
 * рисование без кадра) — RendererError.
 *
 * @note Не потокобезопасен: все вызовы — из потока, создавшего устройство.
 */

#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Image.hpp>
#include <RendererSystem/RHI/Types.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace RendererSystem::RHI {

class Device {
public:
    /**
     * @brief Создаёт устройство выбранного бэкенда.
     *
     * OpenGL: нужен текущий контекст 3.3 core с загруженными функциями (окно WindowSystem с ClientApi::OpenGL).
     * Vulkan: окно не обязательно — без `create_surface` устройство рисует только в текстуры (тесты, офлайн-рендер).
     */
    [[nodiscard]] static std::expected<std::unique_ptr<Device>, std::string> create(const DeviceConfig& config);

    virtual ~Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] Backend backend() const noexcept { return m_info.backend; }
    [[nodiscard]] const DeviceInfo& info() const noexcept { return m_info; }
    [[nodiscard]] const DeviceStats& stats() const noexcept { return m_stats; }

    // ------------------------------------------------------------------ буферы

    /// @brief Пустой буфер.
    [[nodiscard]] virtual BufferId create_buffer(BufferKind kind, BufferUsage usage) = 0;
    /// @brief Заменяет всё содержимое (размер может меняться). Безопасно и после того, как буфер уже рисовался в этом кадре.
    virtual void update_buffer(BufferId buffer, std::span<const std::byte> data) = 0;
    virtual void destroy_buffer(BufferId buffer) = 0;

    // ------------------------------------------------------------------ текстуры и цели

    /// @brief Текстура RGBA8; `pixels` — width·height цветов, строки сверху вниз (пусто — не задано).
    [[nodiscard]] virtual TextureId create_texture(int width, int height, const TextureDesc& desc, std::span<const Color> pixels) = 0;
    /// @brief Заменяет пиксели (размер тот же).
    virtual void update_texture(TextureId texture, std::span<const Color> pixels) = 0;
    /// @brief Перестраивает mip-уровни (после рисования в цель). Без mipmaps — ничего.
    virtual void generate_mipmaps(TextureId texture) = 0;
    virtual void destroy_texture(TextureId texture) = 0;

    /// @brief Цель рендера (рендер в текстуру).
    [[nodiscard]] virtual std::expected<TargetId, std::string> create_target(int width, int height, const TargetDesc& desc) = 0;
    /// @brief Цветовая текстура цели (принадлежит цели).
    [[nodiscard]] virtual TextureId target_texture(TargetId target) const = 0;
    virtual void destroy_target(TargetId target) = 0;
    /// @brief Область UV, при которой текстура цели выглядит так, как была нарисована.
    [[nodiscard]] virtual UvRect target_uv() const noexcept = 0;

    // ------------------------------------------------------------------ конвейеры

    /// @brief Компилирует шейдеры и создаёт конвейер; ошибка — текст с логом компилятора.
    [[nodiscard]] virtual std::expected<PipelineId, std::string> create_pipeline(const PipelineDesc& desc) = 0;
    virtual void destroy_pipeline(PipelineId pipeline) = 0;

    // ------------------------------------------------------------------ кадр

    /**
     * @brief Начало кадра; `width`×`height` — размер экрана (Vulkan пересоздаёт swapchain при изменении).
     * @return false — кадр рисовать некуда (окно свёрнуто); end_frame() всё равно нужен.
     */
    virtual bool begin_frame(int width, int height) = 0;
    /// @brief Конец кадра: Vulkan отправляет работу и показывает кадр; GL — ничего (буферы меняет окно).
    virtual void end_frame() = 0;

    /// @brief Куда рисовать: цель или экран (`{}`); область вывода — вся цель.
    virtual void bind_target(TargetId target) = 0;
    /// @brief Область вывода в текущей цели.
    virtual void set_viewport(int width, int height) = 0;
    /// @brief Очистка текущей цели: цвет (если задан) и глубина.
    virtual void clear(std::optional<Color> color, bool depth) = 0;

    /// @brief Кладёт блок uniform в память кадра; результат можно передать во много вызовов.
    [[nodiscard]] virtual UniformSlice push_uniforms(std::span<const std::byte> data) = 0;
    /// @brief То же для одного значения.
    template<typename T>
    [[nodiscard]] UniformSlice push_uniform(const T& value) {
        return push_uniforms(std::as_bytes(std::span<const T>(&value, 1)));
    }

    /// @brief Рисует.
    virtual void draw(const DrawCall& call) = 0;

    // ------------------------------------------------------------------ чтение

    /// @brief Пиксели цели (строка 0 — верх). Синхронизирует CPU и GPU.
    [[nodiscard]] virtual Image read_target(TargetId target) = 0;
    /// @brief Пиксели экрана текущего кадра (вызывать до end_frame()).
    [[nodiscard]] virtual Image read_screen() = 0;
    /// @brief Ждёт окончания всей работы GPU.
    virtual void wait_idle() = 0;

    /// @brief Сколько сообщений слоёв валидации было (Vulkan с validation; иначе 0).
    [[nodiscard]] virtual std::size_t validation_messages() const noexcept { return 0; }

protected:
    Device() = default;
    DeviceInfo m_info{};
    DeviceStats m_stats{};
};

} // namespace RendererSystem::RHI
