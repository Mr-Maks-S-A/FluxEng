#if defined(FLUX_WINDOW_VULKAN)
#    include <vulkan/vulkan.h> // до GLFW: тогда glfw3.h объявит glfwCreateWindowSurface
#endif
#include "GlfwKeyMap.hpp"

#include <WindowSystem/Backend.hpp>

#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>

namespace WindowSystem {

namespace {

using namespace InputSystem;

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
        if (glfwInit() != GLFW_TRUE) return false;
    }
    ++g_open_windows;
    return true;
}

void release_platform() {
    if (g_open_windows > 0 && --g_open_windows == 0) glfwTerminate();
}

class GlfwBackend final : public IWindowBackend {
public:
    GlfwBackend(GLFWwindow* handle, IBackendSink& sink, ClientApi api) : m_handle(handle), m_sink(sink), m_api(api) {
        glfwSetWindowUserPointer(handle, this);
        install_callbacks();
    }

    ~GlfwBackend() override {
        if (m_handle != nullptr) {
            glfwDestroyWindow(m_handle);
            release_platform();
        }
    }

    GlfwBackend(const GlfwBackend&) = delete;
    GlfwBackend& operator=(const GlfwBackend&) = delete;

    void poll_events() override {
        glfwPollEvents();
        poll_gamepads();
    }
    void swap_buffers() override {
        if (m_api == ClientApi::OpenGL) glfwSwapBuffers(m_handle);
    }
    [[nodiscard]] bool should_close() const noexcept override { return glfwWindowShouldClose(m_handle) == GLFW_TRUE; }
    void request_close() noexcept override { glfwSetWindowShouldClose(m_handle, GLFW_TRUE); }
    void set_title(const std::string& title) override { glfwSetWindowTitle(m_handle, title.c_str()); }

    [[nodiscard]] Size window_size() const noexcept override {
        Size size;
        glfwGetWindowSize(m_handle, &size.width, &size.height);
        return size;
    }
    [[nodiscard]] Size framebuffer_size() const noexcept override {
        Size size;
        glfwGetFramebufferSize(m_handle, &size.width, &size.height);
        return size;
    }
    [[nodiscard]] float content_scale() const noexcept override {
        float x = 1.0f, y = 1.0f;
        glfwGetWindowContentScale(m_handle, &x, &y);
        return x;
    }

    void set_vsync(bool enabled) noexcept override {
        if (m_api != ClientApi::OpenGL) return; // у Vulkan vsync — режим показа swapchain'а
        GLFWwindow* previous = glfwGetCurrentContext();
        glfwMakeContextCurrent(m_handle);
        glfwSwapInterval(enabled ? 1 : 0);
        glfwMakeContextCurrent(previous);
    }

    void set_cursor_mode(CursorMode mode) noexcept override {
        const int value = mode == CursorMode::Captured ? GLFW_CURSOR_DISABLED : mode == CursorMode::Hidden ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL;
        glfwSetInputMode(m_handle, GLFW_CURSOR, value);
        if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
            glfwSetInputMode(m_handle, GLFW_RAW_MOUSE_MOTION, mode == CursorMode::Captured ? GLFW_TRUE : GLFW_FALSE);
        }
    }

    [[nodiscard]] Vec2d cursor_position() const noexcept override {
        Vec2d p;
        glfwGetCursorPos(m_handle, &p.x, &p.y);
        return p;
    }

    [[nodiscard]] void* native_handle() const noexcept override { return m_handle; }

    [[nodiscard]] std::expected<std::uint64_t, std::string> create_vulkan_surface([[maybe_unused]] std::uintptr_t instance) const override {
        if (m_api != ClientApi::None) return std::unexpected(std::string("window has an OpenGL context; create it with ClientApi::None"));
#if defined(FLUX_WINDOW_VULKAN)
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        const VkResult result = glfwCreateWindowSurface(reinterpret_cast<VkInstance>(instance), m_handle, nullptr, &surface);
        if (result != VK_SUCCESS) return std::unexpected(std::format("glfwCreateWindowSurface failed: VkResult {}", static_cast<int>(result)));
        return (std::uint64_t)surface; // на 64 бит — указатель, на 32 — uint64_t: приведение в стиле C подходит обоим
#else
        return std::unexpected(std::string("WindowSystem was built without Vulkan headers"));
#endif
    }

private:
    static GlfwBackend& self(GLFWwindow* w) { return *static_cast<GlfwBackend*>(glfwGetWindowUserPointer(w)); }

    void install_callbacks() {
        glfwSetKeyCallback(m_handle, [](GLFWwindow* w, int key, int, int action, int mods) {
            const InputSystem::Key code = detail::from_glfw_key(key);
            if (code == InputSystem::Key::Unknown) return; // у движка такой клавиши нет
            self(w).m_sink.on_input(KeyInput{code, detail::from_glfw_action(action), detail::from_glfw_mods(mods)});
        });
        glfwSetMouseButtonCallback(m_handle, [](GLFWwindow* w, int button, int action, int mods) {
            if (const auto b = detail::from_glfw_button(button)) {
                self(w).m_sink.on_input(MouseButtonInput{*b, detail::from_glfw_action(action), detail::from_glfw_mods(mods)});
            }
        });
        glfwSetCursorPosCallback(m_handle, [](GLFWwindow* w, double x, double y) { self(w).m_sink.on_input(CursorInput{x, y}); });
        glfwSetScrollCallback(m_handle, [](GLFWwindow* w, double dx, double dy) { self(w).m_sink.on_input(ScrollInput{dx, dy}); });
        glfwSetCharCallback(m_handle, [](GLFWwindow* w, unsigned int c) { self(w).m_sink.on_input(CharInput{c}); });
        glfwSetFramebufferSizeCallback(m_handle, [](GLFWwindow* w, int width, int height) { self(w).m_sink.on_framebuffer_resized(width, height); });
        glfwSetWindowFocusCallback(m_handle, [](GLFWwindow* w, int focused) { self(w).m_sink.on_input(FocusInput{focused == GLFW_TRUE}); });
    }

    /// Геймпады GLFW не присылают событий — опрашиваем после glfwPollEvents и отдаём только изменения.
    void poll_gamepads() {
        for (std::size_t pad = 0; pad < kMaxGamepads; ++pad) {
            const int jid = GLFW_JOYSTICK_1 + static_cast<int>(pad);
            const bool present = glfwJoystickIsGamepad(jid) == GLFW_TRUE;
            PadMemory& memory = m_pads[pad];
            const auto id = static_cast<std::uint8_t>(pad);
            if (present != memory.present) {
                memory = PadMemory{};
                memory.present = present;
                m_sink.on_input(GamepadConnectionInput{id, present});
            }
            GLFWgamepadstate state;
            if (!present || glfwGetGamepadState(jid, &state) != GLFW_TRUE) continue;
            for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b) {
                const bool down = state.buttons[b] == GLFW_PRESS;
                if (down == memory.buttons[b]) continue;
                memory.buttons[b] = down;
                m_sink.on_input(GamepadButtonInput{id, detail::from_glfw_gamepad_button(b), down ? Transition::Press : Transition::Release});
            }
            for (int a = 0; a < static_cast<int>(kGamepadAxisCount); ++a) {
                // Курки GLFW: −1 (отпущен) … +1 (нажат до упора) → 0…1, как у движка.
                const float value = a >= GLFW_GAMEPAD_AXIS_LEFT_TRIGGER ? (state.axes[a] + 1.0f) * 0.5f : state.axes[a];
                if (std::fabs(value - memory.axes[a]) < 0.002f) continue;
                memory.axes[a] = value;
                m_sink.on_input(GamepadAxisInput{id, static_cast<GamepadAxis>(a), value});
            }
        }
    }

    struct PadMemory {
        bool present = false;
        bool buttons[kGamepadButtonCount] = {};
        float axes[kGamepadAxisCount] = {};
    };

    GLFWwindow* m_handle;
    IBackendSink& m_sink;
    ClientApi m_api;
    PadMemory m_pads[kMaxGamepads];
};

} // namespace

BackendResult create_glfw_backend(const WindowConfig& config, IBackendSink& sink) {
    g_last_error.clear();
    if (!acquire_platform()) return std::unexpected(g_last_error.empty() ? std::string("cannot initialize GLFW") : g_last_error);

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
        std::string error = opengl ? std::format("cannot create a window with OpenGL {}.{} core: {}", config.gl_major, config.gl_minor,
                                                 g_last_error.empty() ? "unknown reason" : g_last_error)
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
    return std::unique_ptr<IWindowBackend>(std::make_unique<GlfwBackend>(handle, sink, config.api));
}

std::vector<std::string> glfw_vulkan_instance_extensions() {
    std::vector<std::string> out;
    if (g_open_windows == 0 || glfwVulkanSupported() != GLFW_TRUE) return out; // GLFW ещё не инициализирован или нет загрузчика
    std::uint32_t count = 0;
    const char** names = glfwGetRequiredInstanceExtensions(&count);
    for (std::uint32_t i = 0; names != nullptr && i < count; ++i) out.emplace_back(names[i]);
    return out;
}

int glfw_open_windows() noexcept { return g_open_windows; }

} // namespace WindowSystem
