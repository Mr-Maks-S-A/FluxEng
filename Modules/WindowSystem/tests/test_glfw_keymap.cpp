// Перевод кодов GLFW в коды движка: единственное место, где GLFW_KEY_* встречаются. Дисплей не нужен.

#include "GlfwKeyMap.hpp"

#include <doctest/doctest.h>

#include <map>

using namespace InputSystem;
using WindowSystem::detail::from_glfw_action;
using WindowSystem::detail::from_glfw_button;
using WindowSystem::detail::from_glfw_key;
using WindowSystem::detail::from_glfw_mods;

TEST_SUITE("WindowSystem.GlfwKeyMap") {

TEST_CASE("буквы, цифры, функциональные клавиши и кейпад") {
    CHECK(from_glfw_key(GLFW_KEY_A) == Key::A);
    CHECK(from_glfw_key(GLFW_KEY_Z) == Key::Z);
    CHECK(from_glfw_key(GLFW_KEY_1) == Key::Num1);
    CHECK(from_glfw_key(GLFW_KEY_9) == Key::Num9);
    CHECK(from_glfw_key(GLFW_KEY_0) == Key::Num0);
    CHECK(from_glfw_key(GLFW_KEY_F1) == Key::F1);
    CHECK(from_glfw_key(GLFW_KEY_F12) == Key::F12);
    CHECK(from_glfw_key(GLFW_KEY_F24) == Key::F24);
    CHECK(from_glfw_key(GLFW_KEY_KP_0) == Key::Kp0);
    CHECK(from_glfw_key(GLFW_KEY_KP_7) == Key::Kp7);
    CHECK(from_glfw_key(GLFW_KEY_KP_ENTER) == Key::KpEnter);
}

TEST_CASE("управляющие клавиши и знаки") {
    CHECK(from_glfw_key(GLFW_KEY_SPACE) == Key::Space);
    CHECK(from_glfw_key(GLFW_KEY_ESCAPE) == Key::Escape);
    CHECK(from_glfw_key(GLFW_KEY_ENTER) == Key::Enter);
    CHECK(from_glfw_key(GLFW_KEY_TAB) == Key::Tab);
    CHECK(from_glfw_key(GLFW_KEY_BACKSPACE) == Key::Backspace);
    CHECK(from_glfw_key(GLFW_KEY_LEFT) == Key::Left);
    CHECK(from_glfw_key(GLFW_KEY_UP) == Key::Up);
    CHECK(from_glfw_key(GLFW_KEY_PAGE_DOWN) == Key::PageDown);
    CHECK(from_glfw_key(GLFW_KEY_LEFT_BRACKET) == Key::LeftBracket);
    CHECK(from_glfw_key(GLFW_KEY_RIGHT_BRACKET) == Key::RightBracket);
    CHECK(from_glfw_key(GLFW_KEY_EQUAL) == Key::Equal);
    CHECK(from_glfw_key(GLFW_KEY_MINUS) == Key::Minus);
    CHECK(from_glfw_key(GLFW_KEY_GRAVE_ACCENT) == Key::GraveAccent);
    CHECK(from_glfw_key(GLFW_KEY_LEFT_SHIFT) == Key::LeftShift);
    CHECK(from_glfw_key(GLFW_KEY_RIGHT_CONTROL) == Key::RightCtrl);
    CHECK(from_glfw_key(GLFW_KEY_RIGHT_SUPER) == Key::RightSuper);
}

TEST_CASE("неизвестные и несуществующие клавиши — Unknown") {
    CHECK(from_glfw_key(GLFW_KEY_UNKNOWN) == Key::Unknown);
    CHECK(from_glfw_key(GLFW_KEY_WORLD_1) == Key::Unknown);
    CHECK(from_glfw_key(GLFW_KEY_F25) == Key::Unknown);
    CHECK(from_glfw_key(-5) == Key::Unknown);
    CHECK(from_glfw_key(99999) == Key::Unknown);
}

TEST_CASE("перебор всех кодов GLFW: отображение инъективно, каждая переведённая клавиша имеет имя") {
    std::map<Key, int> seen;
    for (int code = 0; code <= GLFW_KEY_LAST; ++code) {
        const Key key = from_glfw_key(code);
        if (key == Key::Unknown) continue;
        CAPTURE(code);
        CHECK(to_string(key) != "Unknown");
        CHECK_MESSAGE(seen.emplace(key, code).second, "два кода GLFW дали одну клавишу движка");
    }
    // Отображение полное: каждая названная клавиша движка достижима из какого-нибудь кода GLFW (клавиши не «осиротели»).
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
    CHECK(from_glfw_button(GLFW_MOUSE_BUTTON_LEFT) == MouseButton::Left);
    CHECK(from_glfw_button(GLFW_MOUSE_BUTTON_RIGHT) == MouseButton::Right);
    CHECK(from_glfw_button(GLFW_MOUSE_BUTTON_MIDDLE) == MouseButton::Middle);
    CHECK(from_glfw_button(GLFW_MOUSE_BUTTON_8) == MouseButton::Button8);
    CHECK_FALSE(from_glfw_button(8).has_value());
    CHECK_FALSE(from_glfw_button(-1).has_value());

    CHECK(from_glfw_action(GLFW_PRESS) == Transition::Press);
    CHECK(from_glfw_action(GLFW_RELEASE) == Transition::Release);
    CHECK(from_glfw_action(GLFW_REPEAT) == Transition::Repeat);

    CHECK(from_glfw_mods(0) == Modifiers::None);
    const Modifiers both = from_glfw_mods(GLFW_MOD_SHIFT | GLFW_MOD_CONTROL);
    CHECK(has_all(both, Modifiers::Shift | Modifiers::Control));
    CHECK_FALSE(has_all(both, Modifiers::Alt));
    CHECK(has_all(from_glfw_mods(GLFW_MOD_CAPS_LOCK | GLFW_MOD_SUPER), Modifiers::CapsLock | Modifiers::Super));
}

} // TEST_SUITE
