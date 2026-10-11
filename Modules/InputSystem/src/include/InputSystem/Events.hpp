#pragma once
/**
 * @file Events.hpp
 * @brief События ввода, как их видит движок: платформа превращает свои события в эти, остальное от платформы не зависит.
 *
 * Имена с суффиксом `Input` (KeyInput, а не KeyEvent) — чтобы не путать с событиями шины `Core::KeyEvent`:
 * то — запись в EventBus, это — сырой ввод от платформы.
 *
 * Все события — простые данные без указателей: их можно записать, воспроизвести (InputLog), отправить по сети.
 */

#include <InputSystem/Keys.hpp>

#include <cstdint>
#include <variant>

namespace InputSystem {

struct KeyInput {
    Key key = Key::Unknown;
    Transition transition = Transition::Press;
    Modifiers mods = Modifiers::None; ///< Модификаторы в момент события (для горячих клавиш; клавиши-модификаторы — отдельные KeyInput).
    friend bool operator==(const KeyInput&, const KeyInput&) = default;
};

struct MouseButtonInput {
    MouseButton button = MouseButton::Left;
    Transition transition = Transition::Press;
    Modifiers mods = Modifiers::None;
    friend bool operator==(const MouseButtonInput&, const MouseButtonInput&) = default;
};

/// @brief Положение курсора в пикселях окна, (0, 0) — левый верх.
struct CursorInput {
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const CursorInput&, const CursorInput&) = default;
};

/// @brief Колесо: dy — вверх положительное.
struct ScrollInput {
    double dx = 0.0;
    double dy = 0.0;
    friend bool operator==(const ScrollInput&, const ScrollInput&) = default;
};

/// @brief Набранный символ Unicode (с учётом раскладки и Shift) — для текстовых полей.
struct CharInput {
    std::uint32_t codepoint = 0;
    friend bool operator==(const CharInput&, const CharInput&) = default;
};

struct FocusInput {
    bool focused = true;
    friend bool operator==(const FocusInput&, const FocusInput&) = default;
};

struct GamepadConnectionInput {
    std::uint8_t pad = 0;
    bool connected = false;
    friend bool operator==(const GamepadConnectionInput&, const GamepadConnectionInput&) = default;
};

struct GamepadButtonInput {
    std::uint8_t pad = 0;
    GamepadButton button = GamepadButton::A;
    Transition transition = Transition::Press;
    friend bool operator==(const GamepadButtonInput&, const GamepadButtonInput&) = default;
};

struct GamepadAxisInput {
    std::uint8_t pad = 0;
    GamepadAxis axis = GamepadAxis::LeftX;
    float value = 0.0f;
    friend bool operator==(const GamepadAxisInput&, const GamepadAxisInput&) = default;
};

using InputEvent = std::variant<KeyInput, MouseButtonInput, CursorInput, ScrollInput, CharInput, FocusInput,
                                GamepadConnectionInput, GamepadButtonInput, GamepadAxisInput>;

} // namespace InputSystem
