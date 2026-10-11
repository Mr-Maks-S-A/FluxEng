#pragma once
/**
 * @file InputState.hpp
 * @brief Состояние ввода за кадр: что зажато, что нажато и отпущено в этом кадре, курсор, колесо, текст, геймпады.
 *
 * ```
 * state.begin_frame();             // сбрасывает «в этом кадре» (нажато/отпущено, дельты, текст)
 * state.apply(event) …             // события платформы (или внедрённые, или из записи)
 * if (state.pressed(Key::Space))   // игровая логика читает только состояние
 * ```
 *
 * Не зависит от окна и GPU: игровая логика, читающая InputState, тестируется без дисплея, а ввод можно подменить записью.
 *
 * Правила:
 * - `pressed` — клавиша опустилась в этом кадре; `released` — отпущена в этом кадре; автоповтор (Repeat) состояние не меняет;
 * - нажатие и отпускание в одном кадре видны обоими флагами `pressed` и `released`, `down` при этом false;
 * - потеря фокуса отпускает всё (иначе после Alt-Tab клавиши «залипают»).
 */

#include <InputSystem/Events.hpp>
#include <InputSystem/Keys.hpp>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>

namespace InputSystem {

struct Vec2d {
    double x = 0.0; ///< По горизонтали, вправо.
    double y = 0.0; ///< По вертикали, вниз.
};

class InputState {
public:
    static constexpr std::size_t text_capacity = 32; ///< Символов за кадр (лишние теряются: так быстро не печатают).

    /// @brief Начало кадра: сбросить всё, что считается «в этом кадре».
    void begin_frame() noexcept;

    /// @brief Применить событие.
    void apply(const InputEvent& event) noexcept;

    /// @brief Отпустить всё зажатое (потеря фокуса, смена окна).
    void release_all() noexcept;

    // ------------------------------------------------------------------ клавиатура

    [[nodiscard]] bool down(Key key) const noexcept { return test(m_keys_down, index_of(key)); }
    [[nodiscard]] bool pressed(Key key) const noexcept { return test(m_keys_pressed, index_of(key)); }
    [[nodiscard]] bool released(Key key) const noexcept { return test(m_keys_released, index_of(key)); }
    /// @brief Хоть одна клавиша опустилась в этом кадре («нажмите любую клавишу»).
    [[nodiscard]] bool any_key_pressed() const noexcept { return m_keys_pressed.any(); }
    /// @brief Зажатые модификаторы (по клавишам Shift/Ctrl/Alt/Super) и состояние CapsLock/NumLock из последнего события.
    [[nodiscard]] Modifiers modifiers() const noexcept;

    // ------------------------------------------------------------------ мышь

    [[nodiscard]] bool mouse_down(MouseButton b) const noexcept { return test(m_buttons_down, static_cast<std::size_t>(b)); }
    [[nodiscard]] bool mouse_pressed(MouseButton b) const noexcept { return test(m_buttons_pressed, static_cast<std::size_t>(b)); }
    [[nodiscard]] bool mouse_released(MouseButton b) const noexcept { return test(m_buttons_released, static_cast<std::size_t>(b)); }

    [[nodiscard]] Vec2d cursor() const noexcept { return m_cursor; }
    [[nodiscard]] Vec2d cursor_delta() const noexcept { return {m_cursor.x - m_cursor_previous.x, m_cursor.y - m_cursor_previous.y}; }
    /// @brief Колесо за кадр.
    [[nodiscard]] Vec2d scroll() const noexcept { return m_scroll; }
    /// @brief Символы, набранные за кадр, по порядку.
    [[nodiscard]] std::span<const std::uint32_t> text() const noexcept { return {m_text.data(), m_text_size}; }
    [[nodiscard]] bool focused() const noexcept { return m_focused; }

    // ------------------------------------------------------------------ геймпады

    [[nodiscard]] bool gamepad_connected(std::size_t pad) const noexcept { return pad < kMaxGamepads && m_pads[pad].connected; }
    [[nodiscard]] bool gamepad_down(std::size_t pad, GamepadButton b) const noexcept { return pad < kMaxGamepads && test(m_pads[pad].down, static_cast<std::size_t>(b)); }
    [[nodiscard]] bool gamepad_pressed(std::size_t pad, GamepadButton b) const noexcept { return pad < kMaxGamepads && test(m_pads[pad].pressed, static_cast<std::size_t>(b)); }
    [[nodiscard]] bool gamepad_released(std::size_t pad, GamepadButton b) const noexcept { return pad < kMaxGamepads && test(m_pads[pad].released, static_cast<std::size_t>(b)); }
    /// @brief Значение оси сейчас: стики −1…+1, курки 0…1. Отключённый геймпад даёт 0.
    [[nodiscard]] float gamepad_axis(std::size_t pad, GamepadAxis a) const noexcept {
        return pad < kMaxGamepads ? m_pads[pad].axes[static_cast<std::size_t>(a)] : 0.0f;
    }
    /// @brief Значение оси в начале кадра (для определения «пересекла порог в этом кадре»).
    [[nodiscard]] float gamepad_axis_previous(std::size_t pad, GamepadAxis a) const noexcept {
        return pad < kMaxGamepads ? m_pads[pad].axes_previous[static_cast<std::size_t>(a)] : 0.0f;
    }

private:
    template<std::size_t N>
    [[nodiscard]] static bool test(const std::bitset<N>& bits, std::size_t i) noexcept {
        return i < N && bits.test(i);
    }

    struct Pad {
        bool connected = false;
        std::bitset<kGamepadButtonCount> down, pressed, released;
        std::array<float, kGamepadAxisCount> axes{};
        std::array<float, kGamepadAxisCount> axes_previous{};
    };

    void apply_key(const KeyInput& e) noexcept;
    void apply_button(const MouseButtonInput& e) noexcept;
    void apply_pad_button(const GamepadButtonInput& e) noexcept;

    std::bitset<kKeyCount> m_keys_down, m_keys_pressed, m_keys_released;
    std::bitset<kMouseButtonCount> m_buttons_down, m_buttons_pressed, m_buttons_released;
    Modifiers m_locks = Modifiers::None;
    Vec2d m_cursor, m_cursor_previous, m_scroll;
    std::array<std::uint32_t, text_capacity> m_text{};
    std::size_t m_text_size = 0;
    bool m_focused = true;
    std::array<Pad, kMaxGamepads> m_pads{};
};

} // namespace InputSystem
