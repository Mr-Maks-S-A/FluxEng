#pragma once
/**
 * @file Controller.hpp
 * @brief Управление редактором мышью: превращает нажатия и движения в правки `Editor`. Без окна и GPU — тестируется событиями.
 *
 * Жесты (левая кнопка):
 * - по узлу — выбрать и тащить (Shift — добавить к выбору); отпустить — одна группа `MoveNode`;
 * - по порту — потянуть новое ребро; отпустить на порту/узле — соединить, в пустоте — отмена;
 * - по занятому входу или ребру — **поднять** конец ребра: оно отключается, его можно перенести (отпустить в пустоте — удалить);
 * - по пустому месту — рамка выделения.
 * Правая кнопка — сдвиг холста; колесо — масштаб к курсору.
 */

#include <RuneEditor/Editor.hpp>

#include <optional>

namespace RuneEditor {

/// @brief Окно на холст: смещение и масштаб. `world = screen / zoom + pan`.
struct ViewCamera {
    Vec2 pan{0.0f, 0.0f};
    float zoom = 1.0f;
    [[nodiscard]] Vec2 to_world(Vec2 screen) const noexcept { return screen / zoom + pan; }
    [[nodiscard]] Vec2 to_screen(Vec2 world) const noexcept { return (world - pan) * zoom; }
    /// @brief Масштаб ×`factor` с неподвижной точкой под курсором; зум ограничен [0.25; 4].
    void zoom_at(Vec2 screen, float factor) noexcept;
    /// @brief Вписывает прямоугольник холста в окно `viewport` (в пикселях) с полем.
    void frame(const Rect& area, Vec2 viewport, float margin = 40.0f) noexcept;
};

enum class Button : std::uint8_t { Left, Right };

class Controller {
public:
    explicit Controller(Editor& editor) : m_editor(&editor) {}

    ViewCamera camera;

    void press(Button button, Vec2 screen, bool shift = false);
    void move(Vec2 screen);
    void release(Button button, Vec2 screen);
    void wheel(float steps, Vec2 screen) { camera.zoom_at(screen, std::pow(1.15f, steps)); }
    /// @brief Ставит узел из палитры в точку экрана и выделяет его; вернёт номер (`no_node` — нельзя).
    NodeId place(Runes::Rune rune, Vec2 screen, std::int32_t value = 0);
    /// @brief Esc: отменяет текущий жест (тащимое возвращается, ребро не создаётся).
    void cancel();

    // --- состояние для отрисовки
    struct Connecting { PortRef from; Vec2 cursor; bool valid = false; };
    [[nodiscard]] const std::optional<Connecting>& connecting() const noexcept { return m_connecting; }
    [[nodiscard]] std::optional<Rect> box() const noexcept; ///< Рамка выделения (в координатах холста).
    [[nodiscard]] const Pick& hover() const noexcept { return m_hover; }
    [[nodiscard]] bool busy() const noexcept { return m_mode != Mode::Idle; }

private:
    enum class Mode : std::uint8_t { Idle, DragNodes, Connect, Pan, Box };
    void finish_connect(Vec2 world);

    Editor* m_editor;
    Mode m_mode = Mode::Idle;
    Vec2 m_grab_world{}, m_grab_screen{}, m_cursor{};
    std::map<NodeId, Vec2> m_grab_positions;
    std::optional<Connecting> m_connecting;
    bool m_group_open = false; ///< Поднятие ребра открывает группу: «отключить + подключить» — один шаг истории.
    bool m_additive = false;
    Pick m_hover;
};

} // namespace RuneEditor
