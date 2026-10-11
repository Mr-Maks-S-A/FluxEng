#include <InputSystem/Keys.hpp>

#include <algorithm>
#include <array>
#include <cctype>

namespace InputSystem {

namespace {

template<typename E>
struct Named {
    E value;
    std::string_view name;
};

constexpr std::array kKeys = std::to_array<Named<Key>>({
    {Key::A, "A"},
    {Key::B, "B"},
    {Key::C, "C"},
    {Key::D, "D"},
    {Key::E, "E"},
    {Key::F, "F"},
    {Key::G, "G"},
    {Key::H, "H"},
    {Key::I, "I"},
    {Key::J, "J"},
    {Key::K, "K"},
    {Key::L, "L"},
    {Key::M, "M"},
    {Key::N, "N"},
    {Key::O, "O"},
    {Key::P, "P"},
    {Key::Q, "Q"},
    {Key::R, "R"},
    {Key::S, "S"},
    {Key::T, "T"},
    {Key::U, "U"},
    {Key::V, "V"},
    {Key::W, "W"},
    {Key::X, "X"},
    {Key::Y, "Y"},
    {Key::Z, "Z"},
    {Key::Num1, "Num1"},
    {Key::Num2, "Num2"},
    {Key::Num3, "Num3"},
    {Key::Num4, "Num4"},
    {Key::Num5, "Num5"},
    {Key::Num6, "Num6"},
    {Key::Num7, "Num7"},
    {Key::Num8, "Num8"},
    {Key::Num9, "Num9"},
    {Key::Num0, "Num0"},
    {Key::Enter, "Enter"},
    {Key::Escape, "Escape"},
    {Key::Backspace, "Backspace"},
    {Key::Tab, "Tab"},
    {Key::Space, "Space"},
    {Key::Minus, "Minus"},
    {Key::Equal, "Equal"},
    {Key::LeftBracket, "LeftBracket"},
    {Key::RightBracket, "RightBracket"},
    {Key::Backslash, "Backslash"},
    {Key::Semicolon, "Semicolon"},
    {Key::Apostrophe, "Apostrophe"},
    {Key::GraveAccent, "GraveAccent"},
    {Key::Comma, "Comma"},
    {Key::Period, "Period"},
    {Key::Slash, "Slash"},
    {Key::CapsLock, "CapsLock"},
    {Key::F1, "F1"},
    {Key::F2, "F2"},
    {Key::F3, "F3"},
    {Key::F4, "F4"},
    {Key::F5, "F5"},
    {Key::F6, "F6"},
    {Key::F7, "F7"},
    {Key::F8, "F8"},
    {Key::F9, "F9"},
    {Key::F10, "F10"},
    {Key::F11, "F11"},
    {Key::F12, "F12"},
    {Key::PrintScreen, "PrintScreen"},
    {Key::ScrollLock, "ScrollLock"},
    {Key::Pause, "Pause"},
    {Key::Insert, "Insert"},
    {Key::Home, "Home"},
    {Key::PageUp, "PageUp"},
    {Key::Delete, "Delete"},
    {Key::End, "End"},
    {Key::PageDown, "PageDown"},
    {Key::Right, "Right"},
    {Key::Left, "Left"},
    {Key::Down, "Down"},
    {Key::Up, "Up"},
    {Key::NumLock, "NumLock"},
    {Key::KpDivide, "KpDivide"},
    {Key::KpMultiply, "KpMultiply"},
    {Key::KpSubtract, "KpSubtract"},
    {Key::KpAdd, "KpAdd"},
    {Key::KpEnter, "KpEnter"},
    {Key::Kp1, "Kp1"},
    {Key::Kp2, "Kp2"},
    {Key::Kp3, "Kp3"},
    {Key::Kp4, "Kp4"},
    {Key::Kp5, "Kp5"},
    {Key::Kp6, "Kp6"},
    {Key::Kp7, "Kp7"},
    {Key::Kp8, "Kp8"},
    {Key::Kp9, "Kp9"},
    {Key::Kp0, "Kp0"},
    {Key::KpDecimal, "KpDecimal"},
    {Key::Menu, "Menu"},
    {Key::KpEqual, "KpEqual"},
    {Key::F13, "F13"},
    {Key::F14, "F14"},
    {Key::F15, "F15"},
    {Key::F16, "F16"},
    {Key::F17, "F17"},
    {Key::F18, "F18"},
    {Key::F19, "F19"},
    {Key::F20, "F20"},
    {Key::F21, "F21"},
    {Key::F22, "F22"},
    {Key::F23, "F23"},
    {Key::F24, "F24"},
    {Key::LeftCtrl, "LeftCtrl"},
    {Key::LeftShift, "LeftShift"},
    {Key::LeftAlt, "LeftAlt"},
    {Key::LeftSuper, "LeftSuper"},
    {Key::RightCtrl, "RightCtrl"},
    {Key::RightShift, "RightShift"},
    {Key::RightAlt, "RightAlt"},
    {Key::RightSuper, "RightSuper"},
});

constexpr std::array kMouse = std::to_array<Named<MouseButton>>({
    {MouseButton::Left, "Left"}, {MouseButton::Right, "Right"}, {MouseButton::Middle, "Middle"}, {MouseButton::Button4, "Button4"},
    {MouseButton::Button5, "Button5"}, {MouseButton::Button6, "Button6"}, {MouseButton::Button7, "Button7"}, {MouseButton::Button8, "Button8"},
});

constexpr std::array kPadButtons = std::to_array<Named<GamepadButton>>({
    {GamepadButton::A, "A"}, {GamepadButton::B, "B"}, {GamepadButton::X, "X"}, {GamepadButton::Y, "Y"},
    {GamepadButton::LeftBumper, "LeftBumper"}, {GamepadButton::RightBumper, "RightBumper"}, {GamepadButton::Back, "Back"},
    {GamepadButton::Start, "Start"}, {GamepadButton::Guide, "Guide"}, {GamepadButton::LeftThumb, "LeftThumb"},
    {GamepadButton::RightThumb, "RightThumb"}, {GamepadButton::DpadUp, "DpadUp"}, {GamepadButton::DpadRight, "DpadRight"},
    {GamepadButton::DpadDown, "DpadDown"}, {GamepadButton::DpadLeft, "DpadLeft"},
});

constexpr std::array kPadAxes = std::to_array<Named<GamepadAxis>>({
    {GamepadAxis::LeftX, "LeftX"}, {GamepadAxis::LeftY, "LeftY"}, {GamepadAxis::RightX, "RightX"}, {GamepadAxis::RightY, "RightY"},
    {GamepadAxis::LeftTrigger, "LeftTrigger"}, {GamepadAxis::RightTrigger, "RightTrigger"},
});

bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

template<typename E, std::size_t N>
std::string_view name_of(const std::array<Named<E>, N>& table, E value) noexcept {
    for (const auto& entry : table) {
        if (entry.value == value) return entry.name;
    }
    return "Unknown";
}

template<typename E, std::size_t N>
std::optional<E> find_name(const std::array<Named<E>, N>& table, std::string_view name) noexcept {
    for (const auto& entry : table) {
        if (iequals(entry.name, name)) return entry.value;
    }
    return std::nullopt;
}

} // namespace

std::string_view to_string(Key key) noexcept { return name_of(kKeys, key); }
std::string_view to_string(MouseButton button) noexcept { return name_of(kMouse, button); }
std::string_view to_string(GamepadButton button) noexcept { return name_of(kPadButtons, button); }
std::string_view to_string(GamepadAxis axis) noexcept { return name_of(kPadAxes, axis); }

std::optional<Key> parse_key(std::string_view name) noexcept { return find_name(kKeys, name); }
std::optional<MouseButton> parse_mouse_button(std::string_view name) noexcept { return find_name(kMouse, name); }
std::optional<GamepadButton> parse_gamepad_button(std::string_view name) noexcept { return find_name(kPadButtons, name); }
std::optional<GamepadAxis> parse_gamepad_axis(std::string_view name) noexcept { return find_name(kPadAxes, name); }

} // namespace InputSystem
