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
 */

#include <Core/FixedStep.hpp>
#include <Core/PlatformEvents.hpp>

#include <EventSystem/EventSystem.hpp>
#include <RendererSystem/RendererSystem.hpp>
#include <WindowSystem/Window.hpp> // окно, ввод; подключает glad + GLFW

#include <array>
#include <optional>
#include <string>

namespace Core {

class App;

/// @brief Параметры запуска.
struct AppConfig {
    std::string title = "FluxEng";         ///< Заголовок окна и префикс файла графа событий.
    int width = 1280;                      ///< Ширина окна.
    int height = 720;                      ///< Высота окна.
    double ticks_per_second = 30.0;        ///< Частота симуляции при скорости x1.
    int max_frames = -1;                   ///< Выйти через N кадров (для smoke-тестов); -1 — не выходить.
    int max_ticks = -1;                    ///< Выйти через N тиков; включает lockstep: один тик на кадр,
                                           ///< поэтому прогон детерминирован и не зависит от скорости машины.
    std::string screenshot{};              ///< Сохранить последний кадр в PNG (пусто — не сохранять).
};

/// @brief Разбирает `--frames N`, `--ticks N` и `--screenshot file.png` из командной строки.
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

    /// @brief Размер мира — для начальной камеры.
    [[nodiscard]] virtual glm::vec2 world_size() const = 0;

    /// @brief Объявить модули и события, получить писателей/читателей, создать текстуры.
    virtual void setup(App& app) = 0;

    /// @brief Один тик симуляции. После него App вызывает EventBus::advance_tick().
    virtual void tick(App& app) = 0;

    /// @brief Отрисовка мира (между begin/end, камера мира).
    virtual void render(App& app, RendererSystem::Renderer2D& renderer) = 0;

    /// @brief Отрисовка поверх, в пикселях экрана (панели, индикаторы).
    virtual void render_overlay(App& /*app*/, RendererSystem::Renderer2D& /*renderer*/) {}

    /// @brief Строка состояния для заголовка окна.
    [[nodiscard]] virtual std::string status() const { return {}; }
};

/**
 * @brief Владеет окном, рендером и шиной; крутит цикл кадров и тиков.
 *
 * Управление (общее для всех игр):
 * - Space — пауза, `=` / `-` — скорость x1…x8;
 * - WASD / стрелки — сдвиг камеры, колесо — зум к курсору;
 * - F1 — напечатать граф событий и статистику каналов; Esc — выход.
 *
 * Ввод берётся из WindowSystem: клавиши приходят подпиской на `events().key`
 * (и уходят в шину как `platform.key`), мышь и колесо — из `input()` за кадр.
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
    [[nodiscard]] RendererSystem::Camera2D& camera() noexcept { return m_camera; }
    [[nodiscard]] WindowSystem::Window& window() noexcept { return m_window; }
    [[nodiscard]] const FrameInput& input() const noexcept { return m_input; }

    /// @brief Модуль "Platform": производитель KeyEvent и MouseButtonEvent.
    [[nodiscard]] EventSystem::ModuleId platform_module() const noexcept { return m_platform; }

    /// @brief Длительность тика симуляции в игровых секундах (не зависит от скорости).
    [[nodiscard]] float tick_seconds() const noexcept { return static_cast<float>(m_step.tick_seconds()); }
    /// @brief Номер текущего тика.
    [[nodiscard]] EventSystem::Tick tick() const noexcept { return m_bus.current_tick(); }
    /// @brief Доля пути к следующему тику, [0, 1): отрисовка может интерполировать движение.
    [[nodiscard]] float tick_alpha() const noexcept { return m_step.alpha(); }

    /// @brief Печатает граф событий, предупреждения и статистику каналов.
    void print_event_report() const;

private:
    void on_key(int key, int action);
    void poll_input(float frame_seconds);
    void draw_bus_overlay();
    void update_title(Game& game, double fps);
    void save_screenshot(int width, int height) const;

    AppConfig m_config;
    WindowSystem::Window m_window;                         // окно (и GL-контекст) живёт дольше рендера
    std::optional<RendererSystem::Renderer2D> m_renderer;
    EventSystem::EventBus m_bus;
    RendererSystem::Camera2D m_camera;
    FrameInput m_input;

    EventSystem::ModuleId m_platform;
    EventSystem::EventWriter<KeyEvent> m_key_out;
    EventSystem::EventWriter<MouseButtonEvent> m_mouse_out;

    FixedStep m_step;
};

} // namespace Core
