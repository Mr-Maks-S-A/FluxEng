#pragma once
/**
 * @file Backend.hpp
 * @brief Шов платформы: интерфейс бэкенда окна. Другая платформа (SDL, Android, консоль, веб) — ещё одна реализация.
 *
 * Window — фасад: хранит состояние ввода (InputSystem::InputState), рассылает события подписчикам и просит бэкенд
 * сделать то, что умеет только ОС. Бэкенд делает три вещи:
 *  1. при poll_events() переводит события платформы в InputSystem::InputEvent и отдаёт их IBackendSink — больше ничего
 *     о платформе (коды клавиш, действия, модификаторы) наружу не течёт;
 *  2. отвечает на запросы окна: размеры, заголовок, курсор, vsync, поверхность Vulkan;
 *  3. владеет ресурсами платформы (дескриптор окна, контекст).
 *
 * Встроенные реализации: GLFW (`create_glfw_backend`) и Headless (`create_headless_backend`).
 */

#include <InputSystem/Events.hpp>
#include <InputSystem/InputState.hpp>
#include <WindowSystem/Types.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace WindowSystem {

/// @brief Куда бэкенд отдаёт события. Реализует Window.
class IBackendSink {
public:
    virtual ~IBackendSink() = default;
    virtual void on_input(const InputSystem::InputEvent& event) = 0;
    virtual void on_framebuffer_resized(int width, int height) = 0;
};

class IWindowBackend {
public:
    virtual ~IWindowBackend() = default;

    /// @brief Забрать события платформы и отдать их приёмнику (по порядку).
    virtual void poll_events() = 0;
    virtual void swap_buffers() = 0;

    [[nodiscard]] virtual bool should_close() const noexcept = 0;
    virtual void request_close() noexcept = 0;
    virtual void set_title(const std::string& title) = 0;

    [[nodiscard]] virtual Size window_size() const noexcept = 0;
    [[nodiscard]] virtual Size framebuffer_size() const noexcept = 0;
    [[nodiscard]] virtual float content_scale() const noexcept = 0;

    virtual void set_vsync(bool enabled) noexcept = 0;
    virtual void set_cursor_mode(CursorMode mode) noexcept = 0;
    /// @brief Положение курсора сейчас, в пикселях окна (для начального состояния ввода).
    [[nodiscard]] virtual InputSystem::Vec2d cursor_position() const noexcept = 0;

    /// @brief Платформенный дескриптор (для GLFW — GLFWwindow*); nullptr, если его нет.
    [[nodiscard]] virtual void* native_handle() const noexcept = 0;
    [[nodiscard]] virtual std::expected<std::uint64_t, std::string> create_vulkan_surface(std::uintptr_t instance) const = 0;
};

using BackendResult = std::expected<std::unique_ptr<IWindowBackend>, std::string>;

/// @brief Окно ОС через GLFW (и контекст OpenGL, если config.api == OpenGL).
[[nodiscard]] BackendResult create_glfw_backend(const WindowConfig& config, IBackendSink& sink);
/// @brief Окно без экрана: события только внедрённые, графики нет.
[[nodiscard]] BackendResult create_headless_backend(const WindowConfig& config, IBackendSink& sink);

/// @brief Расширения экземпляра Vulkan, нужные платформе (пусто, пока GLFW не инициализирован или нет загрузчика Vulkan).
[[nodiscard]] std::vector<std::string> glfw_vulkan_instance_extensions();
/// @brief Сколько окон GLFW открыто (GLFW инициализируется с первым и завершается с последним).
[[nodiscard]] int glfw_open_windows() noexcept;

} // namespace WindowSystem
