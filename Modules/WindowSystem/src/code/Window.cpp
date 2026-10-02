#if defined(FLUX_WINDOW_VULKAN)
#    include <vulkan/vulkan.h> // до GLFW: тогда glfw3.h объявит glfwCreateWindowSurface
#endif
#include <WindowSystem/Window.hpp>

#include <cstdlib>
#include <format>
#include <string>
#include <utility>

namespace WindowSystem {

struct Window::Impl {
    GLFWwindow* handle = nullptr;
    std::string title;
    bool vsync = true;
    bool close_on_escape = false;
    ClientApi api = ClientApi::OpenGL;
    CursorMode cursor_mode = CursorMode::Normal;
    InputState input;
    WindowEvents events;

    // Общая точка для событий ОС и внедрённых: сначала input, потом подписчики.
    void key(int key_code, int action) {
        input.on_key(key_code, action);
        if (close_on_escape && key_code == GLFW_KEY_ESCAPE && action == GLFW_PRESS && handle != nullptr) {
            glfwSetWindowShouldClose(handle, GLFW_TRUE);
        }
        events.key.emit(key_code, action);
    }
    void mouse_button(int button, int action) {
        input.on_mouse_button(button, action);
        events.mouse_button.emit(button, action);
    }
    void cursor(double x, double y) {
        input.on_cursor(x, y);
        events.cursor.emit(x, y);
    }
    void scroll(double dx, double dy) {
        input.on_scroll(dx, dy);
        events.scroll.emit(dx, dy);
    }
    void character(std::uint32_t codepoint) {
        input.on_char(codepoint);
        events.character.emit(codepoint);
    }
};

namespace {

// Число открытых окон: GLFW инициализируется с первым и завершается с последним.
int g_open_windows = 0;
std::string g_last_error;

void on_glfw_error(int code, const char* description) {
    g_last_error = std::format("GLFW error {}: {}", code, description != nullptr ? description : "?");
}

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool acquire_platform() {
    if (g_open_windows == 0) {
        // Шум GTK/GLib/AT-SPI на Linux (на Windows ничего не делает).
        set_env("G_MESSAGES_DEBUG", "");
        set_env("NO_AT_BRIDGE", "1");
        glfwSetErrorCallback(on_glfw_error);
        if (glfwInit() != GLFW_TRUE) {
            return false;
        }
    }
    ++g_open_windows;
    return true;
}

void release_platform() {
    if (g_open_windows > 0 && --g_open_windows == 0) {
        glfwTerminate();
    }
}

Window::Impl* impl_of(GLFWwindow* handle) { return static_cast<Window::Impl*>(glfwGetWindowUserPointer(handle)); }

void install_callbacks(GLFWwindow* handle) {
    glfwSetKeyCallback(handle, [](GLFWwindow* w, int key, int, int action, int) { impl_of(w)->key(key, action); });
    glfwSetMouseButtonCallback(handle,
                               [](GLFWwindow* w, int button, int action, int) { impl_of(w)->mouse_button(button, action); });
    glfwSetCursorPosCallback(handle, [](GLFWwindow* w, double x, double y) { impl_of(w)->cursor(x, y); });
    glfwSetScrollCallback(handle, [](GLFWwindow* w, double dx, double dy) { impl_of(w)->scroll(dx, dy); });
    glfwSetCharCallback(handle, [](GLFWwindow* w, unsigned int c) { impl_of(w)->character(c); });
    glfwSetFramebufferSizeCallback(handle, [](GLFWwindow* w, int width, int height) {
        impl_of(w)->events.framebuffer_resized.emit(width, height);
    });
    glfwSetWindowFocusCallback(handle, [](GLFWwindow* w, int focused) {
        Window::Impl* impl = impl_of(w);
        if (focused == GLFW_FALSE) impl->input.on_focus_lost();
        impl->events.focus.emit(focused == GLFW_TRUE);
    });
}

} // namespace

Window::Window() noexcept = default;

std::expected<Window, std::string> Window::create(const WindowConfig& config) {
    g_last_error.clear();
    if (!acquire_platform()) {
        return std::unexpected(g_last_error.empty() ? std::string("cannot initialize GLFW") : g_last_error);
    }

    const bool opengl = config.api == ClientApi::OpenGL;
    glfwDefaultWindowHints();
    if (opengl) {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, config.gl_major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, config.gl_minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Vulkan: без контекста OpenGL
    }
    glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SAMPLES, config.samples);

    GLFWwindow* handle = glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
    if (handle == nullptr) {
        std::string error = opengl ? std::format("cannot create a window with OpenGL {}.{} core: {}", config.gl_major,
                                                 config.gl_minor, g_last_error.empty() ? "unknown reason" : g_last_error)
                                   : std::format("cannot create a window: {}", g_last_error.empty() ? "unknown reason" : g_last_error);
        release_platform(); // счётчик возвращается, даже если окно не создалось
        return std::unexpected(std::move(error));
    }

    if (opengl) {
        glfwMakeContextCurrent(handle);
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
            glfwDestroyWindow(handle);
            release_platform();
            return std::unexpected(std::string("cannot load OpenGL functions (glad)"));
        }
    }

    Window window;
    window.m_impl = std::make_unique<Impl>();
    Impl& impl = *window.m_impl;
    impl.handle = handle;
    impl.title = config.title;
    impl.close_on_escape = config.close_on_escape;
    impl.api = config.api;
    glfwSetWindowUserPointer(handle, &impl);
    install_callbacks(handle);
    window.set_vsync(config.vsync);

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(handle, &x, &y);
    impl.input.on_cursor(x, y);
    impl.input.begin_frame();
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

Window::~Window() {
    if (m_impl && m_impl->handle != nullptr) {
        glfwDestroyWindow(m_impl->handle);
        m_impl->handle = nullptr;
        release_platform();
    }
}

void Window::poll_events() {
    if (!m_impl || m_impl->handle == nullptr) return;
    m_impl->input.begin_frame();
    glfwPollEvents();
}

void Window::swap_buffers() {
    if (m_impl && m_impl->handle != nullptr && m_impl->api == ClientApi::OpenGL) glfwSwapBuffers(m_impl->handle);
}

bool Window::should_close() const noexcept {
    return !m_impl || m_impl->handle == nullptr || glfwWindowShouldClose(m_impl->handle) == GLFW_TRUE;
}

void Window::request_close() noexcept {
    if (m_impl && m_impl->handle != nullptr) glfwSetWindowShouldClose(m_impl->handle, GLFW_TRUE);
}

void Window::set_title(std::string_view title) {
    if (!m_impl) return;
    m_impl->title.assign(title);
    if (m_impl->handle != nullptr) glfwSetWindowTitle(m_impl->handle, m_impl->title.c_str());
}

const std::string& Window::title() const noexcept {
    static const std::string empty;
    return m_impl ? m_impl->title : empty;
}

Size Window::window_size() const noexcept {
    Size size;
    if (m_impl && m_impl->handle != nullptr) glfwGetWindowSize(m_impl->handle, &size.width, &size.height);
    return size;
}

Size Window::framebuffer_size() const noexcept {
    Size size;
    if (m_impl && m_impl->handle != nullptr) glfwGetFramebufferSize(m_impl->handle, &size.width, &size.height);
    return size;
}

float Window::content_scale() const noexcept {
    float x = 1.0f;
    float y = 1.0f;
    if (m_impl && m_impl->handle != nullptr) glfwGetWindowContentScale(m_impl->handle, &x, &y);
    return x;
}

void Window::set_vsync(bool enabled) noexcept {
    if (!m_impl || m_impl->handle == nullptr) return;
    m_impl->vsync = enabled;
    if (m_impl->api != ClientApi::OpenGL) return; // у Vulkan vsync — режим показа swapchain'а
    GLFWwindow* previous = glfwGetCurrentContext();
    glfwMakeContextCurrent(m_impl->handle);
    glfwSwapInterval(enabled ? 1 : 0);
    glfwMakeContextCurrent(previous);
}

bool Window::vsync() const noexcept { return m_impl && m_impl->vsync; }

double Window::time() noexcept { return glfwGetTime(); }

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

void Window::inject_key(int key, int action) {
    if (m_impl) m_impl->key(key, action);
}
void Window::inject_mouse_button(int button, int action) {
    if (m_impl) m_impl->mouse_button(button, action);
}
void Window::inject_cursor(double x, double y) {
    if (m_impl) m_impl->cursor(x, y);
}
void Window::inject_scroll(double dx, double dy) {
    if (m_impl) m_impl->scroll(dx, dy);
}
void Window::inject_char(std::uint32_t codepoint) {
    if (m_impl) m_impl->character(codepoint);
}

GLFWwindow* Window::native_handle() const noexcept { return m_impl ? m_impl->handle : nullptr; }

void Window::set_cursor_mode(CursorMode mode) noexcept {
    if (!m_impl || m_impl->handle == nullptr) return;
    m_impl->cursor_mode = mode;
    const int value = mode == CursorMode::Captured ? GLFW_CURSOR_DISABLED : mode == CursorMode::Hidden ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL;
    glfwSetInputMode(m_impl->handle, GLFW_CURSOR, value);
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(m_impl->handle, GLFW_RAW_MOUSE_MOTION, mode == CursorMode::Captured ? GLFW_TRUE : GLFW_FALSE);
    }
}

CursorMode Window::cursor_mode() const noexcept { return m_impl ? m_impl->cursor_mode : CursorMode::Normal; }

ClientApi Window::api() const noexcept { return m_impl ? m_impl->api : ClientApi::OpenGL; }

std::vector<std::string> Window::vulkan_instance_extensions() {
    std::vector<std::string> out;
    if (g_open_windows == 0 || glfwVulkanSupported() != GLFW_TRUE) return out; // GLFW ещё не инициализирован или нет загрузчика
    std::uint32_t count = 0;
    const char** names = glfwGetRequiredInstanceExtensions(&count);
    for (std::uint32_t i = 0; names != nullptr && i < count; ++i) out.emplace_back(names[i]);
    return out;
}

std::expected<std::uint64_t, std::string> Window::create_vulkan_surface([[maybe_unused]] std::uintptr_t instance) const {
    if (!m_impl || m_impl->handle == nullptr) return std::unexpected(std::string("window is not created"));
    if (m_impl->api != ClientApi::None) return std::unexpected(std::string("window has an OpenGL context; create it with ClientApi::None"));
#if defined(FLUX_WINDOW_VULKAN)
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result = glfwCreateWindowSurface(reinterpret_cast<VkInstance>(instance), m_impl->handle, nullptr, &surface);
    if (result != VK_SUCCESS) return std::unexpected(std::format("glfwCreateWindowSurface failed: VkResult {}", static_cast<int>(result)));
    return (std::uint64_t)surface; // на 64 бит — указатель, на 32 — uint64_t: приведение в стиле C подходит обоим
#else
    return std::unexpected(std::string("WindowSystem was built without Vulkan headers"));
#endif
}

int Window::open_windows() noexcept { return g_open_windows; }

} // namespace WindowSystem
