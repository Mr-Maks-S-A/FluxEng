// Перевод кодов SDL3 в коды движка: единственное место, где SDL_SCANCODE_* встречаются. Дисплей не нужен.

#include "SdlKeyMap.hpp"

#include <doctest/doctest.h>

#include <map>

using namespace InputSystem;
using WindowSystem::detail::from_sdl_axis_value;
using WindowSystem::detail::from_sdl_button;
using WindowSystem::detail::from_sdl_gamepad_axis;
using WindowSystem::detail::from_sdl_gamepad_button;
using WindowSystem::detail::from_sdl_key_action;
using WindowSystem::detail::from_sdl_mods;
using WindowSystem::detail::from_sdl_scancode;
using WindowSystem::detail::next_utf8_codepoint;

TEST_SUITE("WindowSystem.SdlKeyMap") {

TEST_CASE("буквы, цифры, функциональные и кейпад") {
    CHECK(from_sdl_scancode(SDL_SCANCODE_A) == Key::A);
    CHECK(from_sdl_scancode(SDL_SCANCODE_Z) == Key::Z);
    CHECK(from_sdl_scancode(SDL_SCANCODE_1) == Key::Num1);
    CHECK(from_sdl_scancode(SDL_SCANCODE_9) == Key::Num9);
    CHECK(from_sdl_scancode(SDL_SCANCODE_0) == Key::Num0);
    CHECK(from_sdl_scancode(SDL_SCANCODE_F1) == Key::F1);
    CHECK(from_sdl_scancode(SDL_SCANCODE_F12) == Key::F12);
    CHECK(from_sdl_scancode(SDL_SCANCODE_F24) == Key::F24);
    CHECK(from_sdl_scancode(SDL_SCANCODE_KP_0) == Key::Kp0);
    CHECK(from_sdl_scancode(SDL_SCANCODE_KP_7) == Key::Kp7);
    CHECK(from_sdl_scancode(SDL_SCANCODE_KP_ENTER) == Key::KpEnter);
}

TEST_CASE("знаки, управляющие и модификаторы") {
    CHECK(from_sdl_scancode(SDL_SCANCODE_SPACE) == Key::Space);
    CHECK(from_sdl_scancode(SDL_SCANCODE_ESCAPE) == Key::Escape);
    CHECK(from_sdl_scancode(SDL_SCANCODE_RETURN) == Key::Enter);
    CHECK(from_sdl_scancode(SDL_SCANCODE_TAB) == Key::Tab);
    CHECK(from_sdl_scancode(SDL_SCANCODE_BACKSPACE) == Key::Backspace);
    CHECK(from_sdl_scancode(SDL_SCANCODE_LEFT) == Key::Left);
    CHECK(from_sdl_scancode(SDL_SCANCODE_UP) == Key::Up);
    CHECK(from_sdl_scancode(SDL_SCANCODE_PAGEDOWN) == Key::PageDown);
    CHECK(from_sdl_scancode(SDL_SCANCODE_LEFTBRACKET) == Key::LeftBracket);
    CHECK(from_sdl_scancode(SDL_SCANCODE_RIGHTBRACKET) == Key::RightBracket);
    CHECK(from_sdl_scancode(SDL_SCANCODE_EQUALS) == Key::Equal);
    CHECK(from_sdl_scancode(SDL_SCANCODE_MINUS) == Key::Minus);
    CHECK(from_sdl_scancode(SDL_SCANCODE_GRAVE) == Key::GraveAccent);
    CHECK(from_sdl_scancode(SDL_SCANCODE_LSHIFT) == Key::LeftShift);
    CHECK(from_sdl_scancode(SDL_SCANCODE_RCTRL) == Key::RightCtrl);
    CHECK(from_sdl_scancode(SDL_SCANCODE_RGUI) == Key::RightSuper);
    CHECK(from_sdl_scancode(SDL_SCANCODE_APPLICATION) == Key::Menu);
}

TEST_CASE("неизвестные и выход за диапазон") {
    CHECK(from_sdl_scancode(SDL_SCANCODE_UNKNOWN) == Key::Unknown);
    CHECK(from_sdl_scancode(SDL_SCANCODE_NONUSBACKSLASH) == Key::Unknown);
    CHECK(from_sdl_scancode(SDL_SCANCODE_VOLUMEUP) == Key::Unknown);
    CHECK(from_sdl_scancode(-5) == Key::Unknown);
    CHECK(from_sdl_scancode(99999) == Key::Unknown);
}

TEST_CASE("перебор всех скан-кодов SDL: отображение инъективно, каждая переведённая клавиша имеет имя") {
    std::map<Key, int> seen;
    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code) {
        const Key key = from_sdl_scancode(code);
        if (key == Key::Unknown) continue;
        CAPTURE(code);
        CHECK(to_string(key) != "Unknown");
        CHECK_MESSAGE(seen.emplace(key, code).second, "два скан-кода SDL дали одну клавишу движка");
    }
    // Каждая названная клавиша движка достижима из какого-нибудь скан-кода SDL (клавиши не «осиротели»).
    int named = 0;
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        const Key key = static_cast<Key>(i);
        if (to_string(key) == "Unknown") continue;
        ++named;
        CAPTURE(to_string(key));
        CHECK(seen.contains(key));
    }
    CHECK(named == 117);
    CHECK(seen.size() == 117);
}

TEST_CASE("кнопки мыши, действия и модификаторы") {
    CHECK(from_sdl_button(SDL_BUTTON_LEFT) == MouseButton::Left);
    CHECK(from_sdl_button(SDL_BUTTON_RIGHT) == MouseButton::Right);
    CHECK(from_sdl_button(SDL_BUTTON_MIDDLE) == MouseButton::Middle);
    CHECK(from_sdl_button(SDL_BUTTON_X1) == MouseButton::Button4);
    CHECK(from_sdl_button(SDL_BUTTON_X2) == MouseButton::Button5);
    CHECK_FALSE(from_sdl_button(0).has_value());
    CHECK_FALSE(from_sdl_button(9).has_value());

    CHECK(from_sdl_key_action(true, false) == Transition::Press);
    CHECK(from_sdl_key_action(false, false) == Transition::Release);
    CHECK(from_sdl_key_action(true, true) == Transition::Repeat);

    CHECK(from_sdl_mods(SDL_KMOD_NONE) == Modifiers::None);
    const Modifiers both = from_sdl_mods(SDL_KMOD_LSHIFT | SDL_KMOD_RCTRL);
    CHECK(has_all(both, Modifiers::Shift | Modifiers::Control));
    CHECK_FALSE(has_all(both, Modifiers::Alt));
    CHECK(has_all(from_sdl_mods(SDL_KMOD_CAPS | SDL_KMOD_LGUI), Modifiers::CapsLock | Modifiers::Super));
}

TEST_CASE("геймпад: кнопки приводятся к раскладке «как у Xbox», оси и курки") {
    CHECK(from_sdl_gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH) == GamepadButton::A);
    CHECK(from_sdl_gamepad_button(SDL_GAMEPAD_BUTTON_WEST) == GamepadButton::X);
    CHECK(from_sdl_gamepad_button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) == GamepadButton::LeftBumper);
    CHECK(from_sdl_gamepad_button(SDL_GAMEPAD_BUTTON_START) == GamepadButton::Start);
    CHECK(from_sdl_gamepad_button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) == GamepadButton::DpadRight);
    CHECK_FALSE(from_sdl_gamepad_button(SDL_GAMEPAD_BUTTON_TOUCHPAD).has_value());

    std::map<GamepadButton, int> seen;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (const auto mapped = from_sdl_gamepad_button(b)) CHECK(seen.emplace(*mapped, b).second);
    CHECK(seen.size() == kGamepadButtonCount);

    CHECK(from_sdl_gamepad_axis(SDL_GAMEPAD_AXIS_RIGHTY) == GamepadAxis::RightY);
    CHECK(from_sdl_gamepad_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) == GamepadAxis::RightTrigger);
    CHECK_FALSE(from_sdl_gamepad_axis(99).has_value());
    CHECK(from_sdl_axis_value(GamepadAxis::LeftX, 32767) == doctest::Approx(1.0f));
    CHECK(from_sdl_axis_value(GamepadAxis::LeftX, -32768) == doctest::Approx(-1.0f));
    CHECK(from_sdl_axis_value(GamepadAxis::LeftTrigger, 32767) == doctest::Approx(1.0f));
    CHECK(from_sdl_axis_value(GamepadAxis::RightTrigger, -5) == 0.0f);
}

TEST_CASE("текст SDL (UTF-8) → кодовые точки") {
    const char* text = "aй€😀";
    std::size_t pos = 0;
    CHECK(next_utf8_codepoint(text, pos) == U'a');
    CHECK(next_utf8_codepoint(text, pos) == U'й');
    CHECK(next_utf8_codepoint(text, pos) == U'€');
    CHECK(next_utf8_codepoint(text, pos) == U'😀');
    CHECK(next_utf8_codepoint(text, pos) == 0);
    pos = 0;
    CHECK(next_utf8_codepoint("\xC3", pos) == 0xFFFD); // оборванная последовательность
    pos = 0;
    CHECK(next_utf8_codepoint("\xFF", pos) == 0xFFFD);
    CHECK(pos == 1);
}

} // TEST_SUITE
