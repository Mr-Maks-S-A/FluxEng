#pragma once
/**
 * @file App.hpp
 * @brief Клиент движка: окно + рендер + ввод поверх RuntimeSystem::Runtime (шина, задачи, память, тик, модули).
 *
 * Модуль Core — то, что стоит между main() и игровой логикой клиента. Ядро без графики (шина событий, JobSystem,
 * арены, фиксированный тик, модули с жизненным циклом) живёт в RuntimeSystem и доступно как App::runtime();
 * выделенный сервер использует Runtime напрямую, без App. Игра реализует интерфейс Game, а App владеет окном
 * и рендером и крутит цикл кадров.
 *
 * Модули (RuntimeSystem::Module) добавляются в Game::configure() и работают одинаково в клиенте и на сервере.
 *
 * Два домена времени:
 * - **кадр** (частота монитора): ввод, камера, отрисовка;
 * - **тик симуляции** (фиксированная частота × скорость игры): логика и EventBus::advance_tick().
 *
 * Пауза останавливает тики, но не кадры: камеру можно двигать на паузе.
 *
 * Запуск и остановка (App::run):
 * ```
 * Game::configure (add_module) → Runtime::initialize (declare всех, init по зависимостям) → Game::setup → кадры
 *   → [любой выход, в том числе исключение] wait_idle → Game::shutdown → Module::shutdown в обратном порядке
 * ```
 *
 * Кадр App:
 * ```
 * poll_events → device.begin_frame → Runtime::begin_frame (арена кадра, Module::frame) → Game::frame (ввод кадра,
 *   рендер в текстуры) → Runtime::update: тики (Module::tick → Game::tick → advance_tick)
 *   → clear (цвет + глубина) → Game::render_3d → Game::render (2D, камера мира) → Game::render_overlay (пиксели экрана)
 *   → device.end_frame (Vulkan: показ) → window.swap_buffers (OpenGL)
 * ```
 */

#include <Core/FixedStep.hpp>
#include <Core/PlatformEvents.hpp>

#include <EventSystem/EventSystem.hpp>
#include <JobSystem/JobSystem.hpp>
#include <MemorySystem/MemorySystem.hpp>
#include <RendererSystem/RendererSystem.hpp>
#include <RuntimeSystem/RuntimeSystem.hpp>
#include <InputSystem/InputSystem.hpp>  // собственные коды ввода, состояние, действия
#include <WindowSystem/Window.hpp>       // окно и события; платформенных заголовков в нём нет

#include <array>
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Core {

class App;

/// @brief Параметры запуска.
struct AppConfig {
    std::string title = "FluxEng";         ///< Заголовок окна и префикс файла графа событий.
    int width = 1280;                      ///< Ширина окна.
    int height = 720;                      ///< Высота окна.
    bool visible = true;                   ///< false — скрытое окно (тесты: рендер и ввод без показа на экране).
    double ticks_per_second = 30.0;        ///< Частота симуляции при скорости x1.
    int max_frames = -1;                   ///< Выйти через N кадров (для smoke-тестов); -1 — не выходить.
    int max_ticks = -1;                    ///< Выйти через N тиков; включает lockstep: один тик на кадр,
                                           ///< поэтому прогон детерминирован и не зависит от скорости машины.
    std::string screenshot{};              ///< Сохранить последний кадр в PNG (пусто — не сохранять).
    InputSystem::Key pause_key = InputSystem::Key::Space; ///< Клавиша паузы (3D-игре Space нужен для прыжка); Key::Unknown — без паузы.
    int threads = -1;                      ///< Фоновых потоков JobSystem; -1 — по умолчанию (ядра − 1),
                                           ///< 0 — всё в главном потоке (эталон для сравнения и отладки).
    bool camera_controls = true;           ///< WASD/стрелки и колесо двигают 2D-камеру (3D-игре обычно не нужно).
    std::uint32_t clear_rgba = 0x15151AFF; ///< Цвет фона кадра (0xRRGGBBAA).
    float ui_font_size = 32.0f;            ///< Кегль, в котором запекается шрифт интерфейса (App::ui_font()).
    RendererSystem::Backend backend = RendererSystem::Backend::OpenGL; ///< Графический API (`--backend gl|vulkan`).
    bool validation = false;               ///< Vulkan: слои валидации (`--validation`).
    std::size_t tick_arena_bytes = MemorySystem::MiB(256); ///< Резерв каждой из двух арен тика (виртуальный).
    std::size_t frame_arena_bytes = MemorySystem::MiB(64); ///< Резерв арены кадра (виртуальный).
    bool profile_modules = false;          ///< Мерить время tick() модулей (Runtime::module_stats()).
    std::vector<std::string> extra_args{}; ///< Аргументы, которые Core не разобрал (для самой игры), по порядку.
};

/**
 * @brief Разбирает `--frames N`, `--ticks N`, `--threads N`, `--screenshot file.png`, `--backend gl|vulkan`,
 * `--validation`; остальное кладёт в `extra_args`.
 * @throws std::invalid_argument Неизвестный бэкенд.
 */
[[nodiscard]] AppConfig parse_args(AppConfig config, int argc, char** argv);

/// @brief Состояние мыши за текущий кадр.
struct FrameInput {
    glm::vec2 mouse_screen{0.0f};        ///< Пиксели окна, (0,0) — левый верх.
    glm::vec2 mouse_world{0.0f};         ///< Точка мира под курсором.
    std::array<bool, 3> down{};          ///< Кнопка удерживается (левая, правая, средняя).
    std::array<bool, 3> pressed{};       ///< Нажата в этом кадре.
};

/**
 * @brief Игра или симуляция, запускаемая в App.
 */
class Game {
public:
    virtual ~Game() = default;

    /// @brief Размер мира — для начальной 2D-камеры (3D-игре можно не переопределять).
    [[nodiscard]] virtual glm::vec2 world_size() const { return {1280.0f, 720.0f}; }

    /// @brief Добавить модули движка (`app.add_module<M>(…)`). Вызывается до инициализации модулей;
    /// модули можно создавать в любом порядке — порядок init задают их зависимости.
    virtual void configure(App& /*app*/) {}

    /// @brief Объявить модули и события, получить писателей/читателей, создать текстуры.
    /// Вызывается после инициализации модулей, добавленных в configure(): их сервисы уже готовы.
    virtual void setup(App& app) = 0;

    /**
     * @brief Начало кадра, до тиков и отрисовки: ввод в домене кадра (обзор мышью, выбор объектов),
     * рендер в текстуры (Framebuffer), загрузка данных на GPU. `seconds` — длительность прошлого кадра.
     */
    virtual void frame(App& /*app*/, float /*seconds*/) {}

    /// @brief Один тик симуляции. После него App вызывает EventBus::advance_tick().
    virtual void tick(App& app) = 0;

    /**
     * @brief 3D-сцена: вызывается после очистки цвета и глубины, до 2D.
     * Игра сама вызывает `renderer.begin(camera, environment)` … `renderer.end()`.
     */
    virtual void render_3d(App& /*app*/, RendererSystem::Renderer3D& /*renderer*/) {}

    /// @brief Отрисовка 2D-мира (между begin/end, камера мира).
    virtual void render(App& /*app*/, RendererSystem::Renderer2D& /*renderer*/) {}

    /// @brief Отрисовка поверх, в пикселях экрана (панели, индикаторы).
    virtual void render_overlay(App& /*app*/, RendererSystem::Renderer2D& /*renderer*/) {}

    /// @brief Строка состояния для заголовка окна.
    [[nodiscard]] virtual std::string status() const { return {}; }

    /// @brief Конец работы (после последнего кадра, до отчёта шины): итоги, сводки. Вызывается и при исключении
    /// в цикле (после setup()); GPU к этому моменту уже простаивает. Модули завершаются после него.
    virtual void shutdown(App& /*app*/) {}
};

/**
 * @brief Владеет окном, рендером и шиной; крутит цикл кадров и тиков.
 *
 * Управление (общее для всех игр):
 * - Space (AppConfig::pause_key) — пауза, `=` / `-` — скорость x1…x8;
 * - WASD / стрелки — сдвиг камеры, колесо — зум к курсору;
 * - F1 — напечатать граф событий и статистику каналов; Esc — выход.
 *
 * Ввод берётся из WindowSystem в собственных кодах движка (InputSystem::Key, MouseButton): клавиши и кнопки мыши
 * приходят подпиской на `events().key` / `events().mouse_button` (и уходят в шину как `platform.key` /
 * `platform.mouse_button`, по порядку, включая внедрённые Window::inject_*), курсор и колесо — из `input()` за кадр.
 * GLFW и его коды в Core не используются.
 */
class App {
public:
    /// @throws std::runtime_error Если не удалось создать окно или рендер.
    explicit App(AppConfig config);

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// @brief Запускает игру; возвращает код выхода процесса.
    int run(Game& game);

    /// @brief Ядро без графики: шина, задачи, память, тик, модули. То же, что получает выделенный сервер.
    [[nodiscard]] RuntimeSystem::Runtime& runtime() noexcept { return m_runtime; }

    /// @brief Добавляет модуль в Runtime (только из Game::configure()).
    template<std::derived_from<RuntimeSystem::Module> M, typename... Args>
    M& add_module(Args&&... args) {
        return m_runtime.add<M>(std::forward<Args>(args)...);
    }

    [[nodiscard]] EventSystem::EventBus& bus() noexcept { return m_runtime.bus(); }
    [[nodiscard]] RendererSystem::Renderer2D& renderer() noexcept { return *m_renderer; }
    /// @brief Графическое устройство (RHI): свои конвейеры, сетки, цели рендера игры.
    [[nodiscard]] RendererSystem::RHI::Device& device() noexcept { return *m_device; }
    [[nodiscard]] RendererSystem::Renderer3D& renderer3d() noexcept { return *m_renderer3d; }
    /**
     * @brief Шрифт интерфейса в renderer(): системный TTF с кириллицей (Font::load_system), если найден,
     * иначе встроенный ASCII-шрифт (Font::builtin).
     */
    [[nodiscard]] RendererSystem::FontHandle ui_font() const noexcept { return m_ui_font; }
    /// @brief Жирный вариант шрифта интерфейса (или тот же ui_font(), если жирного нет).
    [[nodiscard]] RendererSystem::FontHandle ui_font_bold() const noexcept { return m_ui_font_bold; }
    [[nodiscard]] RendererSystem::Camera2D& camera() noexcept { return m_camera; }
    [[nodiscard]] WindowSystem::Window& window() noexcept { return m_window; }
    /**
     * @brief Планировщик задач для систем игры (JobSystem).
     *
     * Число фоновых потоков — AppConfig::threads (`--threads N`). Параллельные системы пишут
     * результаты в JobSystem::ChunkBuffers и сливают их в порядке кусков — тогда симуляция
     * одинакова при любом числе потоков, и прогон `--threads 0` служит эталоном.
     */
    [[nodiscard]] JobSystem::Scheduler& jobs() noexcept { return m_runtime.jobs(); }
    [[nodiscard]] const FrameInput& input() const noexcept { return m_input; }

    /// @brief Модуль "Platform": производитель KeyEvent и MouseButtonEvent.
    [[nodiscard]] EventSystem::ModuleId platform_module() const noexcept { return m_platform; }

    /// @brief Длительность тика симуляции в игровых секундах (не зависит от скорости).
    [[nodiscard]] float tick_seconds() const noexcept { return static_cast<float>(m_runtime.step().tick_seconds()); }
    /// @brief Номер текущего тика.
    [[nodiscard]] EventSystem::Tick tick() const noexcept { return m_runtime.current_tick(); }
    /// @brief Доля пути к следующему тику, [0, 1): отрисовка может интерполировать движение.
    [[nodiscard]] float tick_alpha() const noexcept { return m_runtime.step().alpha(); }

    /// @brief Пауза симуляции (Game::frame продолжает вызываться).
    [[nodiscard]] bool paused() const noexcept { return m_runtime.step().paused; }
    /// @brief Поставить / снять паузу.
    void set_paused(bool paused) noexcept { m_runtime.step().paused = paused; }
    /// @brief Множитель скорости симуляции (1…8).
    [[nodiscard]] int speed() const noexcept { return m_runtime.step().speed; }

    /// @brief Параметры запуска (включая `extra_args` для игры).
    [[nodiscard]] const AppConfig& config() const noexcept { return m_config; }

    /**
     * @brief Память текущего тика (MemorySystem::DoubleArena::current()).
     *
     * Всё, что выделено здесь в тике N, читается в тике N+1 через previous_tick_arena()
     * и освобождается (обнуляется) после него — та же модель, что у событий шины.
     */
    [[nodiscard]] MemorySystem::Arena& tick_arena() noexcept { return m_runtime.tick_arena(); }
    /// @brief Память прошлого тика: только чтение.
    [[nodiscard]] const MemorySystem::Arena& previous_tick_arena() const noexcept { return m_runtime.previous_tick_arena(); }
    /// @brief Временная память кадра (отрисовка, оверлей): очищается в начале каждого кадра.
    [[nodiscard]] MemorySystem::Arena& frame_arena() noexcept { return m_runtime.frame_arena(); }

    /// @brief Печатает граф событий, предупреждения и статистику каналов.
    void print_event_report() const;

private:
    void on_key(InputSystem::Key key, InputSystem::Transition transition);
    void poll_input(float frame_seconds);
    void draw_bus_overlay();
    void update_title(Game& game, double fps);
    void save_screenshot(int width, int height) const;
    void load_ui_fonts();
    void bind_screen(int width, int height);

    AppConfig m_config;
    WindowSystem::Window m_window;                         // окно (и GL-контекст / поверхность Vulkan) живёт дольше устройства
    std::unique_ptr<RendererSystem::RHI::Device> m_device; // устройство живёт дольше рендеров и ресурсов игры
    std::optional<RendererSystem::Renderer2D> m_renderer;
    std::optional<RendererSystem::Renderer3D> m_renderer3d;
    std::optional<RendererSystem::RenderTarget> m_offscreen; ///< «Экран» устройства без окна (Vulkan + скрытое окно).
    RendererSystem::FontHandle m_ui_font{};
    RendererSystem::FontHandle m_ui_font_bold{};
    // Объявлен после устройства и рендеров: при уничтожении модули (а с ними их GPU-ресурсы) уходят раньше устройства.
    RuntimeSystem::Runtime m_runtime;
    RendererSystem::Camera2D m_camera;
    FrameInput m_input;

    EventSystem::ModuleId m_platform;
    EventSystem::EventWriter<KeyEvent> m_key_out;
    EventSystem::EventWriter<MouseButtonEvent> m_mouse_out;

};

} // namespace Core
