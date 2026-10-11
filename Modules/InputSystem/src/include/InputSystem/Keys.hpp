#pragma once
/**
 * @file Keys.hpp
 * @brief Собственные коды ввода движка: клавиши, кнопки мыши, геймпад, модификаторы. Никаких GLFW/SDL-значений.
 *
 * **Числа стабильны навсегда.** Они попадают в записи ввода, сохранения настроек, сетевые команды и реплеи, поэтому
 * не меняются между версиями движка и платформами. Значения клавиш — USB HID Usage (страница Keyboard), общий
 * стандарт: A = 4 … Z = 29, Space = 44, LeftShift = 225. Новые клавиши только добавляются.
 *
 * `Key` — это идентичность клавиши в раскладке US («клавиша с буквой W»), так её отдают платформы. Для WASD на других
 * раскладках пользователь переназначает действия через ActionMap (физическая позиция — задача слоя платформы).
 *
 * Имена (`to_string`, `parse_*`) стабильны так же: по ним ActionMap сохраняется в текстовый файл настроек.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace InputSystem {

/// @brief Клавиша клавиатуры. 0 — неизвестная/не клавиша.
enum class Key : std::uint16_t {
    Unknown = 0,
    A = 4,
    B = 5,
    C = 6,
    D = 7,
    E = 8,
    F = 9,
    G = 10,
    H = 11,
    I = 12,
    J = 13,
    K = 14,
    L = 15,
    M = 16,
    N = 17,
    O = 18,
    P = 19,
    Q = 20,
    R = 21,
    S = 22,
    T = 23,
    U = 24,
    V = 25,
    W = 26,
    X = 27,
    Y = 28,
    Z = 29,
    Num1 = 30,
    Num2 = 31,
    Num3 = 32,
    Num4 = 33,
    Num5 = 34,
    Num6 = 35,
    Num7 = 36,
    Num8 = 37,
    Num9 = 38,
    Num0 = 39,
    Enter = 40, ///< Enter / Return основной клавиатуры
    Escape = 41,
    Backspace = 42,
    Tab = 43,
    Space = 44,
    Minus = 45, ///< - _
    Equal = 46, ///< = +
    LeftBracket = 47, ///< [ {
    RightBracket = 48, ///< ] }
    Backslash = 49, ///< \ |
    Semicolon = 51, ///< ; :
    Apostrophe = 52, ///< ' "
    GraveAccent = 53, ///< ` ~
    Comma = 54, ///< , <
    Period = 55, ///< . >
    Slash = 56, ///< / ?
    CapsLock = 57,
    F1 = 58,
    F2 = 59,
    F3 = 60,
    F4 = 61,
    F5 = 62,
    F6 = 63,
    F7 = 64,
    F8 = 65,
    F9 = 66,
    F10 = 67,
    F11 = 68,
    F12 = 69,
    PrintScreen = 70,
    ScrollLock = 71,
    Pause = 72,
    Insert = 73,
    Home = 74,
    PageUp = 75,
    Delete = 76,
    End = 77,
    PageDown = 78,
    Right = 79,
    Left = 80,
    Down = 81,
    Up = 82,
    NumLock = 83,
    KpDivide = 84,
    KpMultiply = 85,
    KpSubtract = 86,
    KpAdd = 87,
    KpEnter = 88,
    Kp1 = 89,
    Kp2 = 90,
    Kp3 = 91,
    Kp4 = 92,
    Kp5 = 93,
    Kp6 = 94,
    Kp7 = 95,
    Kp8 = 96,
    Kp9 = 97,
    Kp0 = 98,
    KpDecimal = 99,
    Menu = 101,
    KpEqual = 103,
    F13 = 104,
    F14 = 105,
    F15 = 106,
    F16 = 107,
    F17 = 108,
    F18 = 109,
    F19 = 110,
    F20 = 111,
    F21 = 112,
    F22 = 113,
    F23 = 114,
    F24 = 115,
    LeftCtrl = 224,
    LeftShift = 225,
    LeftAlt = 226,
    LeftSuper = 227,
    RightCtrl = 228,
    RightShift = 229,
    RightAlt = 230,
    RightSuper = 231,
};

/// @brief Размер таблиц, индексируемых Key (наибольший код — 231).
inline constexpr std::size_t kKeyCount = 256;

/// @brief Кнопка мыши.
enum class MouseButton : std::uint8_t { Left, Right, Middle, Button4, Button5, Button6, Button7, Button8 };
inline constexpr std::size_t kMouseButtonCount = 8;

/// @brief Кнопки геймпада в раскладке «как у Xbox» (платформа приводит свои контроллеры к ней).
enum class GamepadButton : std::uint8_t {
    A, B, X, Y, LeftBumper, RightBumper, Back, Start, Guide, LeftThumb, RightThumb, DpadUp, DpadRight, DpadDown, DpadLeft,
};
inline constexpr std::size_t kGamepadButtonCount = 15;

/// @brief Оси геймпада. Стики: −1…+1, вниз и вправо — положительные (как координаты экрана). Курки: 0…1.
enum class GamepadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger };
inline constexpr std::size_t kGamepadAxisCount = 6;

/// @brief Сколько геймпадов одновременно.
inline constexpr std::size_t kMaxGamepads = 4;

/// @brief Что произошло с кнопкой.
enum class Transition : std::uint8_t {
    Release = 0,
    Press = 1,
    Repeat = 2, ///< Автоповтор ОС при удержании: состояние не меняет, нужен текстовым полям.
};

/// @brief Модификаторы (битовая маска).
enum class Modifiers : std::uint8_t {
    None = 0,
    Shift = 1,
    Control = 2,
    Alt = 4,
    Super = 8,
    CapsLock = 16,
    NumLock = 32,
};

[[nodiscard]] constexpr Modifiers operator|(Modifiers a, Modifiers b) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr Modifiers operator&(Modifiers a, Modifiers b) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
constexpr Modifiers& operator|=(Modifiers& a, Modifiers b) noexcept { return a = a | b; }

/// @brief В `set` включены все модификаторы из `required`.
[[nodiscard]] constexpr bool has_all(Modifiers set, Modifiers required) noexcept { return (set & required) == required; }

// ----------------------------------------------------------------------------- имена

/// @brief Имя вида `"W"`, `"Space"`, `"LeftShift"`; для Unknown и значений вне списка — `"Unknown"`.
[[nodiscard]] std::string_view to_string(Key key) noexcept;
[[nodiscard]] std::string_view to_string(MouseButton button) noexcept;
[[nodiscard]] std::string_view to_string(GamepadButton button) noexcept;
[[nodiscard]] std::string_view to_string(GamepadAxis axis) noexcept;

/// @brief Обратное преобразование; регистр не важен. nullopt — такого имени нет.
[[nodiscard]] std::optional<Key> parse_key(std::string_view name) noexcept;
[[nodiscard]] std::optional<MouseButton> parse_mouse_button(std::string_view name) noexcept;
[[nodiscard]] std::optional<GamepadButton> parse_gamepad_button(std::string_view name) noexcept;
[[nodiscard]] std::optional<GamepadAxis> parse_gamepad_axis(std::string_view name) noexcept;

/// @brief Код клавиши как индекс таблицы; kKeyCount, если значение вне диапазона.
[[nodiscard]] constexpr std::size_t index_of(Key key) noexcept {
    const auto value = static_cast<std::size_t>(key);
    return value < kKeyCount ? value : kKeyCount;
}

// ----------------------------------------------------------------------------- удобства

/// @brief Клавиша — цифра верхнего ряда или кейпада: её значение 0…9, иначе −1.
[[nodiscard]] constexpr int digit_value(Key key) noexcept {
    const auto k = static_cast<int>(key);
    if (k >= static_cast<int>(Key::Num1) && k <= static_cast<int>(Key::Num9)) return k - static_cast<int>(Key::Num1) + 1;
    if (key == Key::Num0) return 0;
    if (k >= static_cast<int>(Key::Kp1) && k <= static_cast<int>(Key::Kp9)) return k - static_cast<int>(Key::Kp1) + 1;
    if (key == Key::Kp0) return 0;
    return -1;
}

/// @brief Цифра верхнего ряда по значению 0…9; Unknown для остального.
[[nodiscard]] constexpr Key key_from_digit(int digit) noexcept {
    if (digit == 0) return Key::Num0;
    if (digit >= 1 && digit <= 9) return static_cast<Key>(static_cast<int>(Key::Num1) + digit - 1);
    return Key::Unknown;
}

/// @brief Буква A…Z: её номер 0…25, иначе −1.
[[nodiscard]] constexpr int letter_index(Key key) noexcept {
    const auto k = static_cast<int>(key);
    return k >= static_cast<int>(Key::A) && k <= static_cast<int>(Key::Z) ? k - static_cast<int>(Key::A) : -1;
}

[[nodiscard]] constexpr bool is_modifier_key(Key key) noexcept {
    return static_cast<int>(key) >= static_cast<int>(Key::LeftCtrl) && static_cast<int>(key) <= static_cast<int>(Key::RightSuper);
}

} // namespace InputSystem
