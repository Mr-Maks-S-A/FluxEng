#include <WindowSystem/Window.hpp>

#include <WindowSystem/Backend.hpp>

#include <chrono>
#include <type_traits>
#include <utility>

namespace WindowSystem {

using namespace InputSystem;

struct Window::Impl final : IBackendSink {
    std::unique_ptr<IWindowBackend> backend;
    std::string title;
    bool vsync = true;
    bool close_on_escape = false;
    ClientApi api = ClientApi::OpenGL;
    WindowBackend kind = WindowBackend::Glfw;
    CursorMode cursor_mode = CursorMode::Normal;
    InputState input;
    WindowEvents events;

    // Общая точка для событий платформы и внедрённых: сначала input, потом подписчики.
    void on_input(const InputEvent& event) override {
        input.apply(event);
        std::visit(
            [this](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, KeyInput>) {
                    if (close_on_escape && e.key == Key::Escape && e.transition == Transition::Press && backend) backend->request_close();
                    events.key.emit(e.key, e.transition);
                } else if constexpr (std::is_same_v<T, MouseButtonInput>) {
                    events.mouse_button.emit(e.button, e.transition);
                } else if constexpr (std::is_same_v<T, CursorInput>) {
                    events.cursor.emit(e.x, e.y);
                } else if constexpr (std::is_same_v<T, ScrollInput>) {
                    events.scroll.emit(e.dx, e.dy);
                } else if constexpr (std::is_same_v<T, CharInput>) {
                    events.character.emit(e.codepoint);
                } else if constexpr (std::is_same_v<T, FocusInput>) {
                    events.focus.emit(e.focused);
                }
            },
            event);
        events.input.emit(event);
    }

    void on_framebuffer_resized(int width, int height) override { events.framebuffer_resized.emit(width, height); }
};

Window::Window() noexcept = default;

std::expected<Window, std::string> Window::create(const WindowConfig& config) {
    auto impl = std::make_unique<Impl>();
    BackendResult backend = config.backend == WindowBackend::Headless ? create_headless_backend(config, *impl) : create_glfw_backend(config, *impl);
    if (!backend) return std::unexpected(std::move(backend.error()));

    impl->backend = std::move(*backend);
    impl->title = config.title;
    impl->close_on_escape = config.close_on_escape;
    impl->kind = config.backend;
    impl->api = config.backend == WindowBackend::Headless ? ClientApi::None : config.api;

    Window window;
    window.m_impl = std::move(impl);
    window.set_vsync(config.vsync);
    window.m_impl->input.apply(CursorInput{window.m_impl->backend->cursor_position().x, window.m_impl->backend->cursor_position().y});
    window.m_impl->input.begin_frame();
    return window;
}

Window::Window(Window&& other) noexcept = default;

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        Window previous(std::move(*this)); // старое окно закроется в конце этой области
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

Window::~Window() = default;

void Window::poll_events() {
    if (!m_impl || !m_impl->backend) return;
    m_impl->input.begin_frame();
    m_impl->backend->poll_events();
}

void Window::swap_buffers() {
    if (m_impl && m_impl->backend && m_impl->api == ClientApi::OpenGL) m_impl->backend->swap_buffers();
}

bool Window::should_close() const noexcept { return !m_impl || !m_impl->backend || m_impl->backend->should_close(); }

void Window::request_close() noexcept {
    if (m_impl && m_impl->backend) m_impl->backend->request_close();
}

void Window::set_title(std::string_view title) {
    if (!m_impl) return;
    m_impl->title.assign(title);
    if (m_impl->backend) m_impl->backend->set_title(m_impl->title);
}

const std::string& Window::title() const noexcept {
    static const std::string empty;
    return m_impl ? m_impl->title : empty;
}

Size Window::window_size() const noexcept { return m_impl && m_impl->backend ? m_impl->backend->window_size() : Size{}; }
Size Window::framebuffer_size() const noexcept { return m_impl && m_impl->backend ? m_impl->backend->framebuffer_size() : Size{}; }
float Window::content_scale() const noexcept { return m_impl && m_impl->backend ? m_impl->backend->content_scale() : 1.0f; }

void Window::set_vsync(bool enabled) noexcept {
    if (!m_impl || !m_impl->backend) return;
    m_impl->vsync = enabled;
    m_impl->backend->set_vsync(enabled);
}

bool Window::vsync() const noexcept { return m_impl && m_impl->vsync; }

double Window::time() noexcept {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

const InputState& Window::input() const noexcept {
    static const InputState empty{};
    return m_impl ? m_impl->input : empty;
}

Vec2d Window::cursor_in_framebuffer() const noexcept {
    const Vec2d cursor = input().cursor();
    const Size window = window_size();
    const Size framebuffer = framebuffer_size();
    if (window.width <= 0 || window.height <= 0) return cursor;
    return {cursor.x * framebuffer.width / window.width, cursor.y * framebuffer.height / window.height};
}

WindowEvents& Window::events() noexcept {
    if (!m_impl) m_impl = std::make_unique<Impl>(); // подписки на пустом окне допустимы
    return m_impl->events;
}

void Window::inject(const InputEvent& event) {
    if (m_impl) m_impl->on_input(event);
}
void Window::inject_key(Key key, Transition transition, Modifiers mods) { inject(KeyInput{key, transition, mods}); }
void Window::inject_mouse_button(MouseButton button, Transition transition) { inject(MouseButtonInput{button, transition, Modifiers::None}); }
void Window::inject_cursor(double x, double y) { inject(CursorInput{x, y}); }
void Window::inject_scroll(double dx, double dy) { inject(ScrollInput{dx, dy}); }
void Window::inject_char(std::uint32_t codepoint) { inject(CharInput{codepoint}); }

void* Window::native_handle() const noexcept { return m_impl && m_impl->backend ? m_impl->backend->native_handle() : nullptr; }
Window::operator bool() const noexcept { return m_impl && m_impl->backend; }

void Window::set_cursor_mode(CursorMode mode) noexcept {
    if (!m_impl || !m_impl->backend) return;
    m_impl->cursor_mode = mode;
    m_impl->backend->set_cursor_mode(mode);
}

CursorMode Window::cursor_mode() const noexcept { return m_impl ? m_impl->cursor_mode : CursorMode::Normal; }
ClientApi Window::api() const noexcept { return m_impl ? m_impl->api : ClientApi::OpenGL; }
WindowBackend Window::backend() const noexcept { return m_impl ? m_impl->kind : WindowBackend::Glfw; }

std::vector<std::string> Window::vulkan_instance_extensions() { return glfw_vulkan_instance_extensions(); }

std::expected<std::uint64_t, std::string> Window::create_vulkan_surface(std::uintptr_t instance) const {
    if (!m_impl || !m_impl->backend) return std::unexpected(std::string("window is not created"));
    return m_impl->backend->create_vulkan_surface(instance);
}

int Window::open_windows() noexcept { return glfw_open_windows(); }

} // namespace WindowSystem
