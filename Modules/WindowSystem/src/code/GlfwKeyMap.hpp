#pragma once
/**
 * @file GlfwKeyMap.hpp
 * @brief Перевод кодов GLFW в собственные коды движка. Единственное место, где они встречаются.
 *
 * Внутренний заголовок WindowSystem (не устанавливается); тестируется отдельно (tests/test_glfw_keymap.cpp).
 */

// clang-format off
#include <glad/glad.h> // строго до GLFW
#include <GLFW/glfw3.h>
// clang-format on

#include <InputSystem/Keys.hpp>

#include <optional>

namespace WindowSystem::detail {

using InputSystem::Key;

/// @brief Клавиша GLFW → Key; Key::Unknown, если у движка такой нет (GLFW_KEY_UNKNOWN, WORLD_1/2 и т.п.).
[[nodiscard]] constexpr Key from_glfw_key(int key) noexcept {
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return static_cast<Key>(static_cast<int>(Key::A) + (key - GLFW_KEY_A));
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) return static_cast<Key>(static_cast<int>(Key::Num1) + (key - GLFW_KEY_1));
    if (key == GLFW_KEY_0) return Key::Num0;
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) return static_cast<Key>(static_cast<int>(Key::F1) + (key - GLFW_KEY_F1));
    if (key >= GLFW_KEY_F13 && key <= GLFW_KEY_F24) return static_cast<Key>(static_cast<int>(Key::F13) + (key - GLFW_KEY_F13));
    if (key >= GLFW_KEY_KP_1 && key <= GLFW_KEY_KP_9) return static_cast<Key>(static_cast<int>(Key::Kp1) + (key - GLFW_KEY_KP_1));
    switch (key) {
        case GLFW_KEY_KP_0: return Key::Kp0;
        case GLFW_KEY_SPACE: return Key::Space;
        case GLFW_KEY_APOSTROPHE: return Key::Apostrophe;
        case GLFW_KEY_COMMA: return Key::Comma;
        case GLFW_KEY_MINUS: return Key::Minus;
        case GLFW_KEY_PERIOD: return Key::Period;
        case GLFW_KEY_SLASH: return Key::Slash;
        case GLFW_KEY_SEMICOLON: return Key::Semicolon;
        case GLFW_KEY_EQUAL: return Key::Equal;
        case GLFW_KEY_LEFT_BRACKET: return Key::LeftBracket;
        case GLFW_KEY_BACKSLASH: return Key::Backslash;
        case GLFW_KEY_RIGHT_BRACKET: return Key::RightBracket;
        case GLFW_KEY_GRAVE_ACCENT: return Key::GraveAccent;
        case GLFW_KEY_ESCAPE: return Key::Escape;
        case GLFW_KEY_ENTER: return Key::Enter;
        case GLFW_KEY_TAB: return Key::Tab;
        case GLFW_KEY_BACKSPACE: return Key::Backspace;
        case GLFW_KEY_INSERT: return Key::Insert;
        case GLFW_KEY_DELETE: return Key::Delete;
        case GLFW_KEY_RIGHT: return Key::Right;
        case GLFW_KEY_LEFT: return Key::Left;
        case GLFW_KEY_DOWN: return Key::Down;
        case GLFW_KEY_UP: return Key::Up;
        case GLFW_KEY_PAGE_UP: return Key::PageUp;
        case GLFW_KEY_PAGE_DOWN: return Key::PageDown;
        case GLFW_KEY_HOME: return Key::Home;
        case GLFW_KEY_END: return Key::End;
        case GLFW_KEY_CAPS_LOCK: return Key::CapsLock;
        case GLFW_KEY_SCROLL_LOCK: return Key::ScrollLock;
        case GLFW_KEY_NUM_LOCK: return Key::NumLock;
        case GLFW_KEY_PRINT_SCREEN: return Key::PrintScreen;
        case GLFW_KEY_PAUSE: return Key::Pause;
        case GLFW_KEY_KP_DECIMAL: return Key::KpDecimal;
        case GLFW_KEY_KP_DIVIDE: return Key::KpDivide;
        case GLFW_KEY_KP_MULTIPLY: return Key::KpMultiply;
        case GLFW_KEY_KP_SUBTRACT: return Key::KpSubtract;
        case GLFW_KEY_KP_ADD: return Key::KpAdd;
        case GLFW_KEY_KP_ENTER: return Key::KpEnter;
        case GLFW_KEY_KP_EQUAL: return Key::KpEqual;
        case GLFW_KEY_LEFT_SHIFT: return Key::LeftShift;
        case GLFW_KEY_LEFT_CONTROL: return Key::LeftCtrl;
        case GLFW_KEY_LEFT_ALT: return Key::LeftAlt;
        case GLFW_KEY_LEFT_SUPER: return Key::LeftSuper;
        case GLFW_KEY_RIGHT_SHIFT: return Key::RightShift;
        case GLFW_KEY_RIGHT_CONTROL: return Key::RightCtrl;
        case GLFW_KEY_RIGHT_ALT: return Key::RightAlt;
        case GLFW_KEY_RIGHT_SUPER: return Key::RightSuper;
        case GLFW_KEY_MENU: return Key::Menu;
        default: return Key::Unknown;
    }
}

/// @brief Кнопка мыши GLFW (0…7) → MouseButton; nullopt для остального.
[[nodiscard]] constexpr std::optional<InputSystem::MouseButton> from_glfw_button(int button) noexcept {
    if (button < 0 || button >= static_cast<int>(InputSystem::kMouseButtonCount)) return std::nullopt;
    // GLFW_MOUSE_BUTTON_1…8 идут подряд с нуля: левая, правая, средняя, 4…8 — в том же порядке, что у MouseButton.
    return static_cast<InputSystem::MouseButton>(button);
}

[[nodiscard]] constexpr InputSystem::Transition from_glfw_action(int action) noexcept {
    return action == GLFW_RELEASE ? InputSystem::Transition::Release
           : action == GLFW_REPEAT ? InputSystem::Transition::Repeat
                                   : InputSystem::Transition::Press;
}

[[nodiscard]] constexpr InputSystem::Modifiers from_glfw_mods(int mods) noexcept {
    using InputSystem::Modifiers;
    Modifiers out = Modifiers::None;
    if ((mods & GLFW_MOD_SHIFT) != 0) out |= Modifiers::Shift;
    if ((mods & GLFW_MOD_CONTROL) != 0) out |= Modifiers::Control;
    if ((mods & GLFW_MOD_ALT) != 0) out |= Modifiers::Alt;
    if ((mods & GLFW_MOD_SUPER) != 0) out |= Modifiers::Super;
    if ((mods & GLFW_MOD_CAPS_LOCK) != 0) out |= Modifiers::CapsLock;
    if ((mods & GLFW_MOD_NUM_LOCK) != 0) out |= Modifiers::NumLock;
    return out;
}

/// @brief Кнопка геймпада GLFW (GLFW_GAMEPAD_BUTTON_*) → GamepadButton: порядок один и тот же (раскладка «как у Xbox»).
[[nodiscard]] constexpr InputSystem::GamepadButton from_glfw_gamepad_button(int button) noexcept { return static_cast<InputSystem::GamepadButton>(button); }

static_assert(GLFW_GAMEPAD_BUTTON_A == 0 && GLFW_GAMEPAD_BUTTON_DPAD_LEFT == 14 && GLFW_GAMEPAD_BUTTON_LAST == 14);
static_assert(static_cast<int>(InputSystem::GamepadButton::LeftBumper) == GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
static_assert(static_cast<int>(InputSystem::GamepadButton::Guide) == GLFW_GAMEPAD_BUTTON_GUIDE);
static_assert(static_cast<int>(InputSystem::GamepadButton::DpadUp) == GLFW_GAMEPAD_BUTTON_DPAD_UP);
static_assert(GLFW_GAMEPAD_AXIS_LEFT_X == 0 && GLFW_GAMEPAD_AXIS_RIGHT_Y == 3 && GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER == 5);
static_assert(GLFW_MOUSE_BUTTON_LEFT == 0 && GLFW_MOUSE_BUTTON_RIGHT == 1 && GLFW_MOUSE_BUTTON_MIDDLE == 2);

} // namespace WindowSystem::detail
