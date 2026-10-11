#pragma once
/**
 * @file Window.hpp
 * @brief Окно: кадр, свойства, ввод, события. Не зависит от платформы — в заголовках нет ни GLFW, ни glad.
 *
 * @code
 * auto created = WindowSystem::Window::create({.title = "Game", .width = 1280, .height = 720});
 * if (!created) { std::println(stderr, "{}", created.error()); return 1; }
 * WindowSystem::Window& window = *created;
 *
 * using InputSystem::Key;
 * window.events().key.subscribe([&](Key key, InputSystem::Transition t) {
 *     if (key == Key::Escape && t == InputSystem::Transition::Press) window.request_close();
 * });
 *
 * while (!window.should_close()) {
 *     window.poll_events();                          // обновляет input() и рассылает события
 *     if (window.input().pressed(Key::Space)) jump();
 *     render(window.framebuffer_size());
 *     window.swap_buffers();
 * }
 * @endcode
 *
 * **Платформа.** `WindowConfig::backend`: Glfw — настоящее окно ОС, Headless — окно без экрана (события только внедрённые:
 * CI, сервер, автотесты ввода). Свои платформы — реализацией IWindowBackend (Backend.hpp).
 *
 * **Ввод.** Платформа превращает свои события в InputSystem::InputEvent; окно применяет их к InputSystem::InputState
 * и рассылает подписчикам. Коды клавиш и кнопок — собственные коды движка (InputSystem::Key, MouseButton).
 *
 * **Графический API.** `ClientApi::OpenGL` (по умолчанию) — контекст OpenGL, функции загружены внутри окна (подключайте
 * `<glad/glad.h>` сами там, где вызываете GL). `ClientApi::None` — окно без контекста для Vulkan: поверхность создаёт
 * create_vulkan_surface(), кадр показывает сам рендер (swap_buffers() ничего не делает).
 *
 * **ZII.** Созданное по умолчанию окно пусто: все запросы безопасны (размеры 0, `should_close() == true`).
 * Окно можно перемещать: состояние лежит в куче, и обработчики платформы всегда видят актуальный объект.
 */

#include <InputSystem/Events.hpp>
#include <InputSystem/InputState.hpp>
#include <InputSystem/Keys.hpp>
#include <WindowSystem/Listeners.hpp>
#include <WindowSystem/Types.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace WindowSystem {

using InputSystem::Vec2d;

/// @brief Подписки на события окна. Порядок: сначала обновляется input(), затем подписчики; typed-подписки — перед общей `input`.
struct WindowEvents {
    Listeners<InputSystem::Key, InputSystem::Transition> key;                 ///< Клавиша.
    Listeners<InputSystem::MouseButton, InputSystem::Transition> mouse_button; ///< Кнопка мыши.
    Listeners<double, double> cursor;         ///< (x, y) в пикселях окна
    Listeners<double, double> scroll;         ///< (dx, dy)
    Listeners<std::uint32_t> character;       ///< Unicode-символ
    Listeners<int, int> framebuffer_resized;  ///< (ширина, высота) framebuffer'а
    Listeners<bool> focus;                    ///< true — получили фокус
    Listeners<const InputSystem::InputEvent&> input; ///< Любое событие ввода (в том числе геймпад): запись, мосты в шину.
};

class Window {
public:
    /// @brief Пустое окно (ZII).
    Window() noexcept;

    /// @brief Создаёт окно выбранной платформы. Ошибка — строка с причиной (нет дисплея, нет нужной версии OpenGL).
    [[nodiscard]] static std::expected<Window, std::string> create(const WindowConfig& config = {});

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    ~Window();

    // ------------------------------------------------------------------ кадр

    /// @brief Начало кадра ввода: сбрасывает «нажато/отпущено в этом кадре», забирает события платформы.
    void poll_events();

    /// @brief Показать кадр (OpenGL). Для Vulkan и Headless ничего не делает.
    void swap_buffers();

    [[nodiscard]] bool should_close() const noexcept;
    void request_close() noexcept;

    // ------------------------------------------------------------------ свойства

    void set_title(std::string_view title);
    [[nodiscard]] const std::string& title() const noexcept;

    [[nodiscard]] Size window_size() const noexcept;
    /// @brief Размер framebuffer'а в пикселях (на HiDPI больше размера окна).
    [[nodiscard]] Size framebuffer_size() const noexcept;
    [[nodiscard]] float content_scale() const noexcept;

    void set_vsync(bool enabled) noexcept;
    [[nodiscard]] bool vsync() const noexcept;

    /// @brief Секунды с первого вызова в процессе (монотонные, не зависят от платформы окна).
    [[nodiscard]] static double time() noexcept;

    // ------------------------------------------------------------------ ввод и события

    /// @brief Состояние ввода текущего кадра.
    [[nodiscard]] const InputSystem::InputState& input() const noexcept;
    /// @brief Курсор в пикселях framebuffer'а (пересчёт из координат окна на HiDPI).
    [[nodiscard]] Vec2d cursor_in_framebuffer() const noexcept;

    void set_cursor_mode(CursorMode mode) noexcept;
    [[nodiscard]] CursorMode cursor_mode() const noexcept;

    [[nodiscard]] WindowEvents& events() noexcept;

    /// @brief Внедрить событие так, будто его прислала платформа (тесты, боты, воспроизведение записи).
    /// Идёт тем же путём: сначала input(), потом подписчики.
    void inject(const InputSystem::InputEvent& event);
    void inject_key(InputSystem::Key key, InputSystem::Transition transition, InputSystem::Modifiers mods = InputSystem::Modifiers::None);
    void inject_mouse_button(InputSystem::MouseButton button, InputSystem::Transition transition);
    void inject_cursor(double x, double y);
    void inject_scroll(double dx, double dy);
    void inject_char(std::uint32_t codepoint);

    // ------------------------------------------------------------------ платформа и графический API

    [[nodiscard]] ClientApi api() const noexcept;
    [[nodiscard]] WindowBackend backend() const noexcept;

    /// @brief Расширения экземпляра Vulkan для поверхности окна (пусто, если платформа не поддерживает или окно ещё не создавалось).
    [[nodiscard]] static std::vector<std::string> vulkan_instance_extensions();

    /// @brief Создаёт поверхность Vulkan для окна, созданного с `ClientApi::None`. Возвращает VkSurfaceKHR как число.
    [[nodiscard]] std::expected<std::uint64_t, std::string> create_vulkan_surface(std::uintptr_t instance) const;

    /// @brief Дескриптор платформы (GLFWwindow* у бэкенда Glfw); nullptr у пустого окна и у Headless.
    /// Нужен только коду, который сам говорит с платформой (тесты контекста, интеграции).
    [[nodiscard]] void* native_handle() const noexcept;
    /// @brief Окно создано (есть бэкенд).
    [[nodiscard]] explicit operator bool() const noexcept;

    /// @brief Сколько окон GLFW открыто (GLFW завершается вместе с последним).
    [[nodiscard]] static int open_windows() noexcept;

    struct Impl; ///< Состояние окна в куче: адрес стабилен при перемещении Window.

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace WindowSystem
