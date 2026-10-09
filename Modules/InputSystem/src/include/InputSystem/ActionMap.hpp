#pragma once
/**
 * @file ActionMap.hpp
 * @brief Привязка действий игры («jump», «move_x», «cast») к клавишам, кнопкам и осям; переназначение и сохранение в текст.
 *
 * Игра спрашивает о ДЕЙСТВИЯХ, а не о клавишах:
 * @code
 * InputSystem::ActionMap actions;
 * actions.bind("jump", Key::Space).bind("jump", GamepadButton::A);
 * actions.bind_keys("move_x", Key::A, Key::D).bind_axis("move_x", GamepadAxis::LeftX);
 * actions.bind("save", Key::S, Modifiers::Control);
 *
 * if (actions.pressed("jump", state)) jump();
 * const float x = actions.value("move_x", state);          // −1…+1: клавиши и стик в одной оси
 * @endcode
 *
 * Игрок переназначает управление, не трогая код игры: `unbind("jump")` и новые `bind`; карту можно сохранить в текст
 * (`to_text`) и загрузить (`from_text`) — файл настроек читаем и правится руками.
 *
 * Карта хранит только привязки; состояние берётся из InputState (его «нажато в этом кадре» тут и используется).
 * Для горячих путей запрашивайте по ActionId (хеш имени считается один раз).
 */

#include <InputSystem/InputState.hpp>
#include <InputSystem/Keys.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace InputSystem {

/// @brief Идентификатор действия: FNV-1a 32 бита от имени.
struct ActionId {
    std::uint32_t value = 0;
    friend constexpr bool operator==(ActionId, ActionId) noexcept = default;
};

[[nodiscard]] constexpr ActionId action_id(std::string_view name) noexcept {
    std::uint32_t hash = 2166136261u;
    for (const char c : name) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 16777619u;
    }
    return ActionId{hash};
}

struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
};

/// @brief Одна привязка действия к источнику ввода.
struct Binding {
    enum class Source : std::uint8_t {
        Key,       ///< Клавиша (+ обязательные модификаторы).
        Mouse,     ///< Кнопка мыши (+ модификаторы).
        PadButton, ///< Кнопка любого геймпада.
        PadAxis,   ///< Ось любого геймпада (мёртвая зона и множитель).
        KeyAxis,   ///< Пара клавиш «минус / плюс»: −1, 0, +1.
    };
    Source source = Source::Key;
    std::uint16_t code = 0;   ///< Key, MouseButton, GamepadButton, GamepadAxis или «минус»-клавиша.
    std::uint16_t code2 = 0;  ///< KeyAxis: «плюс»-клавиша.
    Modifiers required = Modifiers::None;
    float scale = 1.0f;       ///< PadAxis.
    float deadzone = 0.15f;   ///< PadAxis: значения до порога — ноль, дальше растягиваются на 0…1.

    friend bool operator==(const Binding&, const Binding&) = default;
};

class ActionMap {
public:
    /// @brief Порог, с которого аналоговая ось считается «нажатой» (для down/pressed/released).
    static constexpr float kAxisPressThreshold = 0.5f;

    // ------------------------------------------------------------------ привязки (можно звать в любой момент: переназначение)

    ActionMap& bind(std::string_view action, Key key, Modifiers required = Modifiers::None);
    ActionMap& bind(std::string_view action, MouseButton button, Modifiers required = Modifiers::None);
    ActionMap& bind(std::string_view action, GamepadButton button);
    ActionMap& bind_axis(std::string_view action, GamepadAxis axis, float scale = 1.0f, float deadzone = 0.15f);
    ActionMap& bind_keys(std::string_view action, Key negative, Key positive);

    /// @brief Убрать все привязки действия (и само действие). @return было ли оно.
    bool unbind(std::string_view action);
    void clear() noexcept {
        m_actions.clear();
        m_index.clear();
    }

    // ------------------------------------------------------------------ запросы

    [[nodiscard]] bool has(std::string_view action) const noexcept { return find(action_id(action)) != nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return m_actions.size(); }
    [[nodiscard]] std::vector<std::string_view> names() const;
    /// @brief Привязки действия; пустой span, если действия нет.
    [[nodiscard]] std::span<const Binding> bindings(ActionId id) const noexcept;

    [[nodiscard]] bool down(ActionId id, const InputState& state) const noexcept;
    [[nodiscard]] bool pressed(ActionId id, const InputState& state) const noexcept;
    [[nodiscard]] bool released(ActionId id, const InputState& state) const noexcept;
    /// @brief Аналоговое значение −1…+1: привязки складываются, результат ограничивается.
    [[nodiscard]] float value(ActionId id, const InputState& state) const noexcept;
    /// @brief Две оси как вектор; длиннее единицы сжимается до единицы (по диагонали клавиши не быстрее стика).
    [[nodiscard]] Vec2f vec2(ActionId x, ActionId y, const InputState& state) const noexcept;

    [[nodiscard]] bool down(std::string_view a, const InputState& s) const noexcept { return down(action_id(a), s); }
    [[nodiscard]] bool pressed(std::string_view a, const InputState& s) const noexcept { return pressed(action_id(a), s); }
    [[nodiscard]] bool released(std::string_view a, const InputState& s) const noexcept { return released(action_id(a), s); }
    [[nodiscard]] float value(std::string_view a, const InputState& s) const noexcept { return value(action_id(a), s); }
    [[nodiscard]] Vec2f vec2(std::string_view x, std::string_view y, const InputState& s) const noexcept {
        return vec2(action_id(x), action_id(y), s);
    }

    // ------------------------------------------------------------------ сохранение

    /// @brief Текст настроек. Формат (по строке на действие, `#` — комментарий):
    /// ```
    /// jump:   Key:Space Pad:A
    /// move_x: Keys:A,D Axis:LeftX
    /// look_x: Axis:RightX*2~0.2        # множитель 2, мёртвая зона 0.2
    /// save:   Ctrl+Key:S
    /// fire:   Mouse:Left
    /// ```
    [[nodiscard]] std::string to_text() const;
    /// @brief Разбор текста. Ошибка называет номер строки; частично разобранная карта не возвращается.
    [[nodiscard]] static std::expected<ActionMap, std::string> from_text(std::string_view text);

    friend bool operator==(const ActionMap&, const ActionMap&) = default;

private:
    struct Action {
        ActionId id;
        std::string name;
        std::vector<Binding> bindings;
        friend bool operator==(const Action&, const Action&) = default;
    };

    [[nodiscard]] const Action* find(ActionId id) const noexcept;
    Action& get_or_add(std::string_view name);
    void rebuild_index();

    std::vector<Action> m_actions; // порядок создания сохраняется в to_text
    std::vector<std::pair<std::uint32_t, std::uint32_t>> m_index; // (ActionId, номер в m_actions) по возрастанию id: поиск за O(log n)
};

} // namespace InputSystem
