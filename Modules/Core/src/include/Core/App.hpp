#pragma once
/**
 * @file App.hpp
 * @brief Слой приложения движка: окно + рендер + шина событий, фиксированный тик симуляции.
 *
 * Модуль Core — то, что стоит между main() и игровой логикой. Игра реализует
 * интерфейс Game, а App владеет окном, рендером и шиной и крутит цикл.
 *
 * Два домена времени:
 * - **кадр** (частота монитора): ввод, камера, отрисовка;
 * - **тик симуляции** (фиксированная частота × скорость игры): логика и EventBus::advance_tick().
 *
 * Пауза останавливает тики, но не кадры: камеру можно двигать на паузе.
 *
 * Кадр App:
 * ```
 * poll_events → device.begin_frame → Game::frame (ввод кадра, рендер в текстуры) → тики (Game::tick + advance_tick)
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
#include <WindowSystem/Window.hpp> // окно, ввод; подключает glad + GLFW

#include <array>
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
    int pause_key = GLFW_KEY_SPACE;        ///< Клавиша паузы (3D-игре Space нужен для прыжка); 0 — без паузы.
    int threads = -1;                      ///< Фоновых потоков JobSystem; -1 — по умолчанию (ядра − 1),
                                           ///< 0 — всё в главном потоке (эталон для сравнения и отладки).
    bool camera_controls = true;           ///< WASD/стрелки и колесо двигают 2D-камеру (3D-игре обычно не нужно).
    std::uint32_t clear_rgba = 0x15151AFF; ///< Цвет фона кадра (0xRRGGBBAA).
    float ui_font_size = 32.0f;            ///< Кегль, в котором запекается шрифт интерфейса (App::ui_font()).
    RendererSystem::Backend backend = RendererSystem::Backend::OpenGL; ///< Графический API (`--backend gl|vulkan`).
    bool validation = false;               ///< Vulkan: слои валидации (`--validation`).
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

    /// @brief Объявить модули и события, получить писателей/читателей, создать текстуры.
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

    /// @brief Конец работы (после последнего кадра, до отчёта шины): итоги, сводки.
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
 * Ввод берётся из WindowSystem: клавиши и кнопки мыши приходят подпиской на `events().key` /
 * `events().mouse_button` (и уходят в шину как `platform.key` / `platform.mouse_button`, по порядку,
 * включая внедрённые Window::inject_*), курсор и колесо — из `input()` за кадр.
 */
class App {
public:
    /// @throws std::runtime_error Если не удалось создать окно или рендер.
    explicit App(AppConfig config);

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// @brief Запускает игру; возвращает код выхода процесса.
    int run(Game& game);

    [[nodiscard]] EventSystem::EventBus& bus() noexcept { return m_bus; }
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
    [[nodiscard]] JobSystem::Scheduler& jobs() noexcept { return m_jobs; }
    [[nodiscard]] const FrameInput& input() const noexcept { return m_input; }

    /// @brief Модуль "Platform": производитель KeyEvent и MouseButtonEvent.
    [[nodiscard]] EventSystem::ModuleId platform_module() const noexcept { return m_platform; }

    /// @brief Длительность тика симуляции в игровых секундах (не зависит от скорости).
    [[nodiscard]] float tick_seconds() const noexcept { return static_cast<float>(m_step.tick_seconds()); }
    /// @brief Номер текущего тика.
    [[nodiscard]] EventSystem::Tick tick() const noexcept { return m_bus.current_tick(); }
    /// @brief Доля пути к следующему тику, [0, 1): отрисовка может интерполировать движение.
    [[nodiscard]] float tick_alpha() const noexcept { return m_step.alpha(); }

    /// @brief Пауза симуляции (Game::frame продолжает вызываться).
    [[nodiscard]] bool paused() const noexcept { return m_step.paused; }
    /// @brief Поставить / снять паузу.
    void set_paused(bool paused) noexcept { m_step.paused = paused; }
    /// @brief Множитель скорости симуляции (1…8).
    [[nodiscard]] int speed() const noexcept { return m_step.speed; }

    /// @brief Параметры запуска (включая `extra_args` для игры).
    [[nodiscard]] const AppConfig& config() const noexcept { return m_config; }

    /**
     * @brief Память текущего тика (MemorySystem::DoubleArena::current()).
     *
     * Всё, что выделено здесь в тике N, читается в тике N+1 через previous_tick_arena()
     * и освобождается (обнуляется) после него — та же модель, что у событий шины.
     */
    [[nodiscard]] MemorySystem::Arena& tick_arena() noexcept { return m_tick_memory.current(); }
    /// @brief Память прошлого тика: только чтение.
    [[nodiscard]] const MemorySystem::Arena& previous_tick_arena() const noexcept { return m_tick_memory.previous(); }
    /// @brief Временная память кадра (отрисовка, оверлей): очищается в начале каждого кадра.
    [[nodiscard]] MemorySystem::Arena& frame_arena() noexcept { return m_frame_memory; }

    /// @brief Печатает граф событий, предупреждения и статистику каналов.
    void print_event_report() const;

private:
    void on_key(int key, int action);
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
    EventSystem::EventBus m_bus;
    RendererSystem::Camera2D m_camera;
    FrameInput m_input;

    EventSystem::ModuleId m_platform;
    EventSystem::EventWriter<KeyEvent> m_key_out;
    EventSystem::EventWriter<MouseButtonEvent> m_mouse_out;

    FixedStep m_step;
    JobSystem::Scheduler m_jobs;
    MemorySystem::DoubleArena m_tick_memory = MemorySystem::DoubleArena::reserve(MemorySystem::MiB(256), MemorySystem::KiB(64), MemorySystem::MemoryTag::Engine);
    MemorySystem::Arena m_frame_memory = MemorySystem::Arena::reserve(MemorySystem::MiB(64), MemorySystem::KiB(64), MemorySystem::MemoryTag::Scratch);
};

} // namespace Core
