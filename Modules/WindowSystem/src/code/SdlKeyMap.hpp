#pragma once
/**
 * @file SdlKeyMap.hpp
 * @brief Перевод кодов SDL3 в собственные коды движка. Единственное место, где они встречаются.
 *
 * Коды клавиш движка — USB HID Usage (см. InputSystem/Keys.hpp), и SDL_Scancode — тоже они: перевод сводится к проверке,
 * что у движка такая клавиша названа. Кнопки мыши, модификаторы и кнопки геймпада отличаются порядком — для них таблицы.
 *
 * Внутренний заголовок WindowSystem (не устанавливается); тестируется отдельно (tests/test_sdl_keymap.cpp).
 */

#include <SDL3/SDL.h>

#include <InputSystem/Keys.hpp>

#include <cmath>
#include <optional>

namespace WindowSystem::detail {

using InputSystem::Key;

static_assert(static_cast<int>(Key::A) == SDL_SCANCODE_A && static_cast<int>(Key::Z) == SDL_SCANCODE_Z);
static_assert(static_cast<int>(Key::Space) == SDL_SCANCODE_SPACE && static_cast<int>(Key::Escape) == SDL_SCANCODE_ESCAPE);
static_assert(static_cast<int>(Key::F1) == SDL_SCANCODE_F1 && static_cast<int>(Key::F24) == SDL_SCANCODE_F24);
static_assert(static_cast<int>(Key::Kp0) == SDL_SCANCODE_KP_0 && static_cast<int>(Key::Menu) == SDL_SCANCODE_APPLICATION);
static_assert(static_cast<int>(Key::LeftCtrl) == SDL_SCANCODE_LCTRL && static_cast<int>(Key::RightSuper) == SDL_SCANCODE_RGUI);

/// @brief Скан-код SDL → Key; Key::Unknown, если у движка такой клавиши нет (мультимедиа, NONUSBACKSLASH и т.п.).
[[nodiscard]] inline Key from_sdl_scancode(int scancode) noexcept {
    if (scancode <= 0 || scancode >= static_cast<int>(InputSystem::kKeyCount)) return Key::Unknown;
    const auto key = static_cast<Key>(scancode);
    return InputSystem::to_string(key) == "Unknown" ? Key::Unknown : key;
}

/// @brief Кнопка мыши SDL (SDL_BUTTON_LEFT = 1 …) → MouseButton; nullopt для остального.
[[nodiscard]] constexpr std::optional<InputSystem::MouseButton> from_sdl_button(int button) noexcept {
    using InputSystem::MouseButton;
    switch (button) {
        case SDL_BUTTON_LEFT: return MouseButton::Left;
        case SDL_BUTTON_RIGHT: return MouseButton::Right;
        case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
        case SDL_BUTTON_X1: return MouseButton::Button4;
        case SDL_BUTTON_X2: return MouseButton::Button5;
        default: return std::nullopt;
    }
}

/// @brief Нажатие/отпускание/автоповтор клавиши.
[[nodiscard]] constexpr InputSystem::Transition from_sdl_key_action(bool down, bool repeat) noexcept {
    return !down ? InputSystem::Transition::Release : repeat ? InputSystem::Transition::Repeat : InputSystem::Transition::Press;
}

[[nodiscard]] constexpr InputSystem::Modifiers from_sdl_mods(SDL_Keymod mods) noexcept {
    using InputSystem::Modifiers;
    Modifiers out = Modifiers::None;
    if ((mods & SDL_KMOD_SHIFT) != 0) out |= Modifiers::Shift;
    if ((mods & SDL_KMOD_CTRL) != 0) out |= Modifiers::Control;
    if ((mods & SDL_KMOD_ALT) != 0) out |= Modifiers::Alt;
    if ((mods & SDL_KMOD_GUI) != 0) out |= Modifiers::Super;
    if ((mods & SDL_KMOD_CAPS) != 0) out |= Modifiers::CapsLock;
    if ((mods & SDL_KMOD_NUM) != 0) out |= Modifiers::NumLock;
    return out;
}

/// @brief Кнопка геймпада SDL → GamepadButton (раскладка «как у Xbox»); nullopt для остального (тачпад, педали и т.п.).
[[nodiscard]] constexpr std::optional<InputSystem::GamepadButton> from_sdl_gamepad_button(int button) noexcept {
    using InputSystem::GamepadButton;
    switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return GamepadButton::A;
        case SDL_GAMEPAD_BUTTON_EAST: return GamepadButton::B;
        case SDL_GAMEPAD_BUTTON_WEST: return GamepadButton::X;
        case SDL_GAMEPAD_BUTTON_NORTH: return GamepadButton::Y;
        case SDL_GAMEPAD_BUTTON_BACK: return GamepadButton::Back;
        case SDL_GAMEPAD_BUTTON_GUIDE: return GamepadButton::Guide;
        case SDL_GAMEPAD_BUTTON_START: return GamepadButton::Start;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return GamepadButton::LeftThumb;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return GamepadButton::RightThumb;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return GamepadButton::LeftBumper;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return GamepadButton::RightBumper;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return GamepadButton::DpadUp;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return GamepadButton::DpadRight;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return GamepadButton::DpadDown;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return GamepadButton::DpadLeft;
        default: return std::nullopt;
    }
}

/// @brief Ось геймпада SDL → GamepadAxis; nullopt для неизвестной.
[[nodiscard]] constexpr std::optional<InputSystem::GamepadAxis> from_sdl_gamepad_axis(int axis) noexcept {
    using InputSystem::GamepadAxis;
    switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: return GamepadAxis::LeftX;
        case SDL_GAMEPAD_AXIS_LEFTY: return GamepadAxis::LeftY;
        case SDL_GAMEPAD_AXIS_RIGHTX: return GamepadAxis::RightX;
        case SDL_GAMEPAD_AXIS_RIGHTY: return GamepadAxis::RightY;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return GamepadAxis::LeftTrigger;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return GamepadAxis::RightTrigger;
        default: return std::nullopt;
    }
}

/// @brief Значение оси SDL (int16) → число движка: стики −1…+1, курки 0…1.
[[nodiscard]] inline float from_sdl_axis_value(InputSystem::GamepadAxis axis, std::int16_t raw) noexcept {
    const float v = std::fmax(-1.0f, static_cast<float>(raw) / 32767.0f);
    return axis >= InputSystem::GamepadAxis::LeftTrigger ? std::fmax(0.0f, v) : v;
}

/// @brief Первая кодовая точка UTF-8 строки SDL_EVENT_TEXT_INPUT; следующая позиция — в `pos`. 0 — строка закончилась или ошибка.
[[nodiscard]] inline char32_t next_utf8_codepoint(const char* text, std::size_t& pos) noexcept {
    if (text == nullptr || text[pos] == 0) return 0;
    const auto b = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char c = b(pos);
    int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
    if (extra < 0) { ++pos; return 0xFFFD; }
    char32_t cp = extra == 0 ? c : static_cast<char32_t>(c & (0x3F >> extra));
    for (int i = 1; i <= extra; ++i) {
        if (text[pos + static_cast<std::size_t>(i)] == 0 || (b(pos + static_cast<std::size_t>(i)) & 0xC0) != 0x80) { pos += static_cast<std::size_t>(i); return 0xFFFD; }
        cp = (cp << 6) | (b(pos + static_cast<std::size_t>(i)) & 0x3F);
    }
    pos += static_cast<std::size_t>(extra) + 1;
    return cp;
}

} // namespace WindowSystem::detail
