#if defined(FLUX_WINDOW_VULKAN)
#    include <vulkan/vulkan.h> // до SDL: тогда SDL_vulkan.h использует типы Vulkan, а не свои заглушки
#endif
#include "SdlKeyMap.hpp"

#include <glad/glad.h>

#include <SDL3/SDL_vulkan.h>

#include <WindowSystem/Backend.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <utility>

namespace WindowSystem {

namespace {

using namespace InputSystem;

// Число открытых окон: видео и геймпады SDL поднимаются с первым окном и опускаются с последним.
int g_open_windows = 0;
bool g_quit_requested = false; ///< SDL_EVENT_QUIT: закрыть всё
class SdlBackend;
std::map<SDL_WindowID, SdlBackend*> g_windows; ///< Очередь событий SDL общая: окна разбирают её по windowID

bool acquire_platform() {
    if (g_open_windows == 0) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
        SDL_InitSubSystem(SDL_INIT_GAMEPAD); // геймпады необязательны: окно без них работает
        g_quit_requested = false;
    }
    ++g_open_windows;
    return true;
}

void release_platform() {
    if (g_open_windows > 0 && --g_open_windows == 0) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

class SdlBackend final : public IWindowBackend {
public:
    SdlBackend(SDL_Window* handle, SDL_GLContext context, IBackendSink& sink, ClientApi api)
        : m_handle(handle), m_context(context), m_sink(sink), m_api(api), m_id(SDL_GetWindowID(handle)) {
        g_windows[m_id] = this;
        SDL_StartTextInput(handle);
        float x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        m_virtual = {x, y};
    }

    ~SdlBackend() override {
        g_windows.erase(m_id);
        for (SDL_Gamepad*& pad : m_pads) {
            if (pad != nullptr) SDL_CloseGamepad(pad);
        }
        if (m_context != nullptr) SDL_GL_DestroyContext(m_context);
        if (m_handle != nullptr) SDL_DestroyWindow(m_handle);
        release_platform();
    }

    SdlBackend(const SdlBackend&) = delete;
    SdlBackend& operator=(const SdlBackend&) = delete;

    void poll_events() override {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                g_quit_requested = true;
                continue;
            }
            if (is_gamepad_event(e.type)) {
                handle_gamepad(e); // у событий геймпада нет окна: их разбирает тот, кто опрашивает
                continue;
            }
            const SDL_WindowID target = window_of(e);
            const auto it = g_windows.find(target);
            if (it != g_windows.end()) it->second->handle(e);
        }
    }

    void swap_buffers() override {
        if (m_api == ClientApi::OpenGL) SDL_GL_SwapWindow(m_handle);
    }
    void make_current() noexcept override {
        if (m_api == ClientApi::OpenGL) SDL_GL_MakeCurrent(m_handle, m_context);
    }
    [[nodiscard]] bool should_close() const noexcept override { return m_close || g_quit_requested; }
    void request_close() noexcept override { m_close = true; }
    void set_title(const std::string& title) override { SDL_SetWindowTitle(m_handle, title.c_str()); }

    [[nodiscard]] Size window_size() const noexcept override {
        Size size;
        SDL_GetWindowSize(m_handle, &size.width, &size.height);
        return size;
    }
    [[nodiscard]] Size framebuffer_size() const noexcept override {
        Size size;
        SDL_GetWindowSizeInPixels(m_handle, &size.width, &size.height);
        return size;
    }
    [[nodiscard]] float content_scale() const noexcept override {
        const float scale = SDL_GetWindowDisplayScale(m_handle);
        return scale > 0.0f ? scale : 1.0f;
    }

    void set_vsync(bool enabled) noexcept override {
        if (m_api != ClientApi::OpenGL) return; // у Vulkan vsync — режим показа swapchain'а
        SDL_Window* previous_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
        SDL_GL_MakeCurrent(m_handle, m_context);
        SDL_GL_SetSwapInterval(enabled ? 1 : 0);
        if (previous_window != nullptr) SDL_GL_MakeCurrent(previous_window, previous_context);
    }

    void set_cursor_mode(CursorMode mode) noexcept override {
        if (mode == CursorMode::Hidden) SDL_HideCursor();
        else SDL_ShowCursor();
        const bool captured = mode == CursorMode::Captured;
        if (captured && !m_captured) {
            float x = 0, y = 0;
            SDL_GetMouseState(&x, &y);
            m_virtual = {x, y}; // относительный режим продолжает «виртуальную» позицию с того места, где курсор был
        }
        m_captured = captured;
        SDL_SetWindowRelativeMouseMode(m_handle, captured);
    }

    [[nodiscard]] Vec2d cursor_position() const noexcept override {
        if (m_captured) return m_virtual;
        float x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        return {x, y};
    }

    [[nodiscard]] void* native_handle() const noexcept override { return m_handle; }

    [[nodiscard]] std::expected<std::uint64_t, std::string> create_vulkan_surface([[maybe_unused]] std::uintptr_t instance) const override {
        if (m_api != ClientApi::None) return std::unexpected(std::string("window has an OpenGL context; create it with ClientApi::None"));
#if defined(FLUX_WINDOW_VULKAN)
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(m_handle, reinterpret_cast<VkInstance>(instance), nullptr, &surface))
            return std::unexpected(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
        return (std::uint64_t)surface; // на 64 бит — указатель, на 32 — uint64_t: приведение в стиле C подходит обоим
#else
        return std::unexpected(std::string("WindowSystem was built without Vulkan headers"));
#endif
    }

private:
    static bool is_gamepad_event(Uint32 type) noexcept {
        return type == SDL_EVENT_GAMEPAD_ADDED || type == SDL_EVENT_GAMEPAD_REMOVED || type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
               type == SDL_EVENT_GAMEPAD_BUTTON_UP || type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
    }

    static SDL_WindowID window_of(const SDL_Event& e) noexcept {
        switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: return e.key.windowID;
            case SDL_EVENT_TEXT_INPUT: return e.text.windowID;
            case SDL_EVENT_MOUSE_MOTION: return e.motion.windowID;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: return e.button.windowID;
            case SDL_EVENT_MOUSE_WHEEL: return e.wheel.windowID;
            default:
                if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) return e.window.windowID;
                return 0;
        }
    }

    void handle(const SDL_Event& e) {
        switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const Key key = detail::from_sdl_scancode(static_cast<int>(e.key.scancode));
                if (key == Key::Unknown) return; // у движка такой клавиши нет
                m_sink.on_input(KeyInput{key, detail::from_sdl_key_action(e.key.down, e.key.repeat), detail::from_sdl_mods(e.key.mod)});
                break;
            }
            case SDL_EVENT_TEXT_INPUT: {
                std::size_t pos = 0;
                while (char32_t cp = detail::next_utf8_codepoint(e.text.text, pos)) m_sink.on_input(CharInput{static_cast<unsigned int>(cp)});
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
                if (m_captured) {
                    m_virtual.x += static_cast<double>(e.motion.xrel);
                    m_virtual.y += static_cast<double>(e.motion.yrel);
                    m_sink.on_input(CursorInput{m_virtual.x, m_virtual.y});
                } else {
                    m_sink.on_input(CursorInput{static_cast<double>(e.motion.x), static_cast<double>(e.motion.y)});
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (const auto b = detail::from_sdl_button(e.button.button)) {
                    m_sink.on_input(MouseButtonInput{*b, e.button.down ? Transition::Press : Transition::Release, detail::from_sdl_mods(SDL_GetModState())});
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                m_sink.on_input(ScrollInput{static_cast<double>(e.wheel.x), static_cast<double>(e.wheel.y)});
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: m_close = true; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: m_sink.on_framebuffer_resized(e.window.data1, e.window.data2); break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED: m_sink.on_input(FocusInput{true}); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: m_sink.on_input(FocusInput{false}); break;
            default: break;
        }
    }

    /// Геймпады SDL присылают события: слот — порядок подключения (до kMaxGamepads); лишние контроллеры игнорируются.
    void handle_gamepad(const SDL_Event& e) {
        switch (e.type) {
            case SDL_EVENT_GAMEPAD_ADDED: {
                for (std::size_t slot = 0; slot < kMaxGamepads; ++slot) {
                    if (m_pads[slot] != nullptr) continue;
                    SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which);
                    if (pad == nullptr) return;
                    m_pads[slot] = pad;
                    m_pad_ids[slot] = e.gdevice.which;
                    m_sink.on_input(GamepadConnectionInput{static_cast<std::uint8_t>(slot), true});
                    return;
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED: {
                const int slot = slot_of(e.gdevice.which);
                if (slot < 0) return;
                SDL_CloseGamepad(m_pads[slot]);
                m_pads[slot] = nullptr;
                m_sink.on_input(GamepadConnectionInput{static_cast<std::uint8_t>(slot), false});
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const int slot = slot_of(e.gbutton.which);
                const auto button = detail::from_sdl_gamepad_button(e.gbutton.button);
                if (slot < 0 || !button) return;
                m_sink.on_input(GamepadButtonInput{static_cast<std::uint8_t>(slot), *button, e.gbutton.down ? Transition::Press : Transition::Release});
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                const int slot = slot_of(e.gaxis.which);
                const auto axis = detail::from_sdl_gamepad_axis(e.gaxis.axis);
                if (slot < 0 || !axis) return;
                m_sink.on_input(GamepadAxisInput{static_cast<std::uint8_t>(slot), *axis, detail::from_sdl_axis_value(*axis, e.gaxis.value)});
                break;
            }
            default: break;
        }
    }

    [[nodiscard]] int slot_of(SDL_JoystickID id) const noexcept {
        for (std::size_t i = 0; i < kMaxGamepads; ++i)
            if (m_pads[i] != nullptr && m_pad_ids[i] == id) return static_cast<int>(i);
        return -1;
    }

    SDL_Window* m_handle;
    SDL_GLContext m_context;
    IBackendSink& m_sink;
    ClientApi m_api;
    SDL_WindowID m_id;
    bool m_close = false;
    bool m_captured = false;
    Vec2d m_virtual{};
    SDL_Gamepad* m_pads[kMaxGamepads] = {};
    SDL_JoystickID m_pad_ids[kMaxGamepads] = {};
};

} // namespace

BackendResult create_sdl_backend(const WindowConfig& config, IBackendSink& sink) {
    if (!acquire_platform()) return std::unexpected(std::format("cannot initialize SDL: {}", SDL_GetError()));

    const bool opengl = config.api == ClientApi::OpenGL;
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (!config.visible) flags |= SDL_WINDOW_HIDDEN;
    if (config.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (opengl) {
        flags |= SDL_WINDOW_OPENGL;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, config.gl_major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, config.gl_minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, config.samples > 0 ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, config.samples);
    }

    SDL_Window* handle = nullptr;
#if defined(FLUX_WINDOW_VULKAN)
    if (!opengl) handle = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags | SDL_WINDOW_VULKAN);
#endif
    if (handle == nullptr) handle = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    if (handle == nullptr) {
        std::string error = opengl ? std::format("cannot create a window with OpenGL {}.{} core: {}", config.gl_major, config.gl_minor, SDL_GetError())
                                   : std::format("cannot create a window: {}", SDL_GetError());
        release_platform(); // счётчик возвращается, даже если окно не создалось
        return std::unexpected(std::move(error));
    }

    SDL_GLContext context = nullptr;
    if (opengl) {
        context = SDL_GL_CreateContext(handle);
        if (context == nullptr) {
            std::string error = std::format("cannot create an OpenGL {}.{} core context: {}", config.gl_major, config.gl_minor, SDL_GetError());
            SDL_DestroyWindow(handle);
            release_platform();
            return std::unexpected(std::move(error));
        }
        SDL_GL_MakeCurrent(handle, context);
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) == 0) {
            SDL_GL_DestroyContext(context);
            SDL_DestroyWindow(handle);
            release_platform();
            return std::unexpected(std::string("cannot load OpenGL functions (glad)"));
        }
    }
    return std::unique_ptr<IWindowBackend>(std::make_unique<SdlBackend>(handle, context, sink, config.api));
}

std::vector<std::string> sdl_vulkan_instance_extensions() {
    std::vector<std::string> out;
    if (g_open_windows == 0 || !SDL_Vulkan_LoadLibrary(nullptr)) return out; // SDL ещё не инициализирован или нет загрузчика
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    for (Uint32 i = 0; names != nullptr && i < count; ++i) out.emplace_back(names[i]);
    return out;
}

int sdl_open_windows() noexcept { return g_open_windows; }

} // namespace WindowSystem
