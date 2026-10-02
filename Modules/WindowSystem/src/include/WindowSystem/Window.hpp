#pragma once
/**
 * @file Window.hpp
 * @brief Окно поверх GLFW: создание (с OpenGL-контекстом или без него — для Vulkan), кадр, свойства, ввод, события.
 *
 * @code
 * auto created = WindowSystem::Window::create({.title = "Game", .width = 1280, .height = 720});
 * if (!created) { std::println(stderr, "{}", created.error()); return 1; }
 * WindowSystem::Window& window = *created;
 *
 * window.events().key.subscribe([&](int key, int action) {
 *     if (key == GLFW_KEY_ESCAPE && action == WindowSystem::action_press) window.request_close();
 * });
 *
 * while (!window.should_close()) {
 *     window.poll_events();                          // обновляет input() и рассылает события
 *     if (window.input().pressed(GLFW_KEY_SPACE)) jump();
 *     const WindowSystem::Size fb = window.framebuffer_size();
 *     render(fb.width, fb.height);
 *     window.swap_buffers();
 * }
 * @endcode
 *
 * Заголовок подключает glad и GLFW: игре нужны коды клавиш `GLFW_KEY_*` и функции OpenGL.
 *
 * **Графический API.** `WindowConfig::api = ClientApi::OpenGL` (по умолчанию) — окно с контекстом OpenGL,
 * функции загружены glad. `ClientApi::None` — окно без контекста для Vulkan: поверхность создаёт
 * create_vulkan_surface(), кадр показывает сам рендер (swap_buffers() ничего не делает).
 *
 * **ZII.** Созданное по умолчанию окно пусто: все запросы безопасны (размеры 0, `should_close() == true`).
 * Окно можно перемещать: состояние лежит в куче, и обработчики GLFW всегда видят актуальный объект.
 */

#include <WindowSystem/Input.hpp>
#include <WindowSystem/Listeners.hpp>

// clang-format off
#include <glad/glad.h>   // glad — строго до GLFW
#include <GLFW/glfw3.h>
// clang-format on

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace WindowSystem {

/// @brief Размер в пикселях.
struct Size {
    int width = 0;  ///< Ширина.
    int height = 0; ///< Высота.
    /// @brief Совпадают обе стороны.
    friend bool operator==(Size, Size) noexcept = default;
};

/// @brief Для какого графического API создаётся окно.
enum class ClientApi : std::uint8_t {
    OpenGL, ///< Окно с контекстом OpenGL (gl_major.gl_minor core), функции загружены glad.
    None,   ///< Без контекста: для Vulkan (поверхность — create_vulkan_surface()).
};

/// @brief Режим курсора.
enum class CursorMode : std::uint8_t {
    Normal,   ///< Обычный курсор.
    Hidden,   ///< Невидим над окном, но двигается свободно.
    Captured, ///< Скрыт и захвачен: относительное движение без упора в край (обзор мышью в 3D).
};

/// @brief Параметры создания окна.
struct WindowConfig {
    std::string title = "FluxEng"; ///< Заголовок.
    int width = 1280;              ///< Ширина окна (в экранных координатах).
    int height = 720;              ///< Высота окна.
    bool visible = true;           ///< false — скрытое окно (тесты, рендер в текстуру).
    bool resizable = true;         ///< Можно ли менять размер мышью.
    bool vsync = true;             ///< Синхронизация с частотой монитора.
    int gl_major = 3;              ///< Версия OpenGL (Core Profile): старшая.
    int gl_minor = 3;              ///< Версия OpenGL: младшая.
    int samples = 0;               ///< MSAA; 0 — выключено.
    bool close_on_escape = false;  ///< Закрывать окно по Esc (удобно для примеров; в игре решает она сама).
    ClientApi api = ClientApi::OpenGL; ///< OpenGL-контекст или окно для Vulkan.
};

/// @brief События окна. На каждое можно подписать сколько угодно обработчиков.
struct WindowEvents {
    Listeners<int, int> key;                  ///< (GLFW_KEY_*, action)
    Listeners<int, int> mouse_button;         ///< (GLFW_MOUSE_BUTTON_*, action)
    Listeners<double, double> cursor;         ///< (x, y) в пикселях окна
    Listeners<double, double> scroll;         ///< (dx, dy)
    Listeners<std::uint32_t> character;       ///< Unicode-символ
    Listeners<int, int> framebuffer_resized;  ///< (ширина, высота) framebuffer'а
    Listeners<bool> focus;                    ///< true — получили фокус
};

/// @brief Окно (с OpenGL-контекстом или для Vulkan).
class Window {
public:
    /// @brief Пустое окно (ZII): ничего не открыто, запросы безопасны.
    Window() noexcept;

    /**
     * @brief Создаёт окно, делает его контекст текущим и загружает OpenGL (glad).
     * @return Окно или текст ошибки (GLFW не инициализировался, нет нужной версии OpenGL, нет дисплея).
     */
    [[nodiscard]] static std::expected<Window, std::string> create(const WindowConfig& config = {});

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    /// @brief Перемещение: окно и подписки переходят к новому объекту, старый становится пустым.
    Window(Window&& other) noexcept;
    /// @brief Перемещение; прежнее окно этого объекта закрывается.
    Window& operator=(Window&& other) noexcept;
    ~Window();

    // ------------------------------------------------------------------ кадр

    /**
     * @brief Начинает кадр ввода и обрабатывает события ОС.
     *
     * Сначала InputState::begin_frame(), затем `glfwPollEvents()`: обработчики событий
     * вызываются внутри, input() после возврата описывает этот кадр.
     */
    void poll_events();

    /// @brief Показывает нарисованный кадр (OpenGL). Для окна без контекста ничего не делает: кадр показывает рендер.
    void swap_buffers();

    /// @brief Пользователь или программа попросили закрыть окно.
    [[nodiscard]] bool should_close() const noexcept;
    /// @brief Просит закрыть окно (should_close() станет true).
    void request_close() noexcept;

    // ------------------------------------------------------------------ свойства

    /// @brief Меняет заголовок.
    void set_title(std::string_view title);
    /// @brief Текущий заголовок.
    [[nodiscard]] const std::string& title() const noexcept;

    /// @brief Размер окна в экранных координатах (в них приходит курсор).
    [[nodiscard]] Size window_size() const noexcept;
    /// @brief Размер framebuffer'а в пикселях (для glViewport; на HiDPI больше window_size()).
    [[nodiscard]] Size framebuffer_size() const noexcept;
    /// @brief Масштаб контента (1.0 — обычный монитор, 2.0 — Retina/200%).
    [[nodiscard]] float content_scale() const noexcept;

    /// @brief Включает/выключает vsync (для контекста этого окна).
    void set_vsync(bool enabled) noexcept;
    /// @brief Включён ли vsync.
    [[nodiscard]] bool vsync() const noexcept;

    /// @brief Секунды с инициализации GLFW (монотонные часы, высокая точность).
    [[nodiscard]] static double time() noexcept;

    // ------------------------------------------------------------------ ввод и события

    /// @brief Состояние ввода за текущий кадр.
    [[nodiscard]] const InputState& input() const noexcept;
    /// @brief Курсор в пикселях framebuffer'а (с учётом HiDPI) — удобно для камеры.
    [[nodiscard]] Vec2d cursor_in_framebuffer() const noexcept;

    /// @brief Режим курсора; при Captured включается «сырое» движение мыши (без ускорения ОС), если поддерживается.
    void set_cursor_mode(CursorMode mode) noexcept;
    /// @brief Текущий режим курсора.
    [[nodiscard]] CursorMode cursor_mode() const noexcept;

    // ------------------------------------------------------------------ графический API

    /// @brief API, для которого создано окно.
    [[nodiscard]] ClientApi api() const noexcept;

    /// @brief Расширения экземпляра Vulkan, нужные для показа в окна этой платформы (пусто — Vulkan недоступен).
    [[nodiscard]] static std::vector<std::string> vulkan_instance_extensions();

    /**
     * @brief Создаёт VkSurfaceKHR для этого окна.
     * @param instance VkInstance, приведённый к целому (заголовок не требует vulkan.h).
     * @return VkSurfaceKHR как целое или текст ошибки (окно с OpenGL-контекстом, нет поддержки Vulkan).
     */
    [[nodiscard]] std::expected<std::uint64_t, std::string> create_vulkan_surface(std::uintptr_t instance) const;

    /// @brief Подписки на события окна.
    [[nodiscard]] WindowEvents& events() noexcept;

    /**
     * @name Внедрение событий
     * Идут тем же путём, что события ОС: обновляют input() и рассылают подписчикам.
     * Нужны для тестов и воспроизведения записанного ввода (replay).
     * @{
     */
    void inject_key(int key, int action);          ///< Клавиша.
    void inject_mouse_button(int button, int action); ///< Кнопка мыши.
    void inject_cursor(double x, double y);        ///< Курсор.
    void inject_scroll(double dx, double dy);      ///< Колесо.
    void inject_char(std::uint32_t codepoint);     ///< Символ.
    /** @} */

    /// @brief GLFW-окно для того, чего здесь ещё нет (или `nullptr`).
    [[nodiscard]] GLFWwindow* native_handle() const noexcept;
    /// @brief `true`, если окно создано.
    [[nodiscard]] explicit operator bool() const noexcept { return native_handle() != nullptr; }

    /// @brief Сколько окон открыто сейчас (GLFW живёт, пока их больше нуля).
    [[nodiscard]] static int open_windows() noexcept;

    struct Impl; ///< Состояние окна в куче: адрес стабилен при перемещении Window.

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace WindowSystem
