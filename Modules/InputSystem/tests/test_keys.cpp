#include <InputSystem/InputSystem.hpp>

#include <doctest/doctest.h>

using namespace InputSystem;

TEST_SUITE("InputSystem.Keys") {

TEST_CASE("числа кодов стабильны навсегда (они лежат в записях, сохранениях и сетевых командах)") {
    CHECK(static_cast<int>(Key::Unknown) == 0);
    CHECK(static_cast<int>(Key::A) == 4);
    CHECK(static_cast<int>(Key::Z) == 29);
    CHECK(static_cast<int>(Key::Num1) == 30);
    CHECK(static_cast<int>(Key::Num0) == 39);
    CHECK(static_cast<int>(Key::Enter) == 40);
    CHECK(static_cast<int>(Key::Escape) == 41);
    CHECK(static_cast<int>(Key::Space) == 44);
    CHECK(static_cast<int>(Key::F1) == 58);
    CHECK(static_cast<int>(Key::F12) == 69);
    CHECK(static_cast<int>(Key::Right) == 79);
    CHECK(static_cast<int>(Key::Up) == 82);
    CHECK(static_cast<int>(Key::Kp1) == 89);
    CHECK(static_cast<int>(Key::Kp0) == 98);
    CHECK(static_cast<int>(Key::F13) == 104);
    CHECK(static_cast<int>(Key::F24) == 115);
    CHECK(static_cast<int>(Key::LeftCtrl) == 224);
    CHECK(static_cast<int>(Key::LeftShift) == 225);
    CHECK(static_cast<int>(Key::RightSuper) == 231);
    CHECK(static_cast<int>(MouseButton::Left) == 0);
    CHECK(static_cast<int>(MouseButton::Right) == 1);
    CHECK(static_cast<int>(MouseButton::Middle) == 2);
    CHECK(static_cast<int>(GamepadButton::A) == 0);
    CHECK(static_cast<int>(GamepadButton::DpadLeft) == 14);
    CHECK(static_cast<int>(GamepadAxis::RightTrigger) == 5);
    CHECK(static_cast<int>(Transition::Press) == 1);
}

TEST_CASE("все клавиши имеют уникальные имена, обратный разбор возвращает ту же клавишу") {
    int named = 0;
    for (int code = 0; code < 1024; ++code) {
        const Key key = static_cast<Key>(code);
        const std::string_view name = to_string(key);
        if (name == "Unknown") continue;
        ++named;
        CAPTURE(code);
        CAPTURE(name);
        const auto parsed = parse_key(name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == key);
    }
    CHECK(named == 117);
}

TEST_CASE("разбор имён: регистр не важен; чужое имя — nullopt") {
    CHECK(parse_key("w") == Key::W);
    CHECK(parse_key("SPACE") == Key::Space);
    CHECK(parse_key("leftshift") == Key::LeftShift);
    CHECK(parse_key("F13") == Key::F13);
    CHECK_FALSE(parse_key("").has_value());
    CHECK_FALSE(parse_key("Spacebar").has_value());
    CHECK_FALSE(parse_key("Unknown").has_value());
    CHECK(to_string(static_cast<Key>(9999)) == "Unknown");
}

TEST_CASE("кнопки мыши, геймпада и оси: имена туда и обратно") {
    for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
        const auto b = static_cast<MouseButton>(i);
        CHECK(parse_mouse_button(to_string(b)) == b);
    }
    for (std::size_t i = 0; i < kGamepadButtonCount; ++i) {
        const auto b = static_cast<GamepadButton>(i);
        CHECK(parse_gamepad_button(to_string(b)) == b);
    }
    for (std::size_t i = 0; i < kGamepadAxisCount; ++i) {
        const auto a = static_cast<GamepadAxis>(i);
        CHECK(parse_gamepad_axis(to_string(a)) == a);
    }
    CHECK_FALSE(parse_mouse_button("Wheel").has_value());
    CHECK_FALSE(parse_gamepad_axis("LeftZ").has_value());
}

TEST_CASE("цифры, буквы, модификаторы: вспомогательные функции") {
    CHECK(digit_value(Key::Num1) == 1);
    CHECK(digit_value(Key::Num9) == 9);
    CHECK(digit_value(Key::Num0) == 0);
    CHECK(digit_value(Key::Kp7) == 7);
    CHECK(digit_value(Key::Kp0) == 0);
    CHECK(digit_value(Key::A) == -1);
    CHECK(key_from_digit(5) == Key::Num5);
    CHECK(key_from_digit(0) == Key::Num0);
    CHECK(key_from_digit(10) == Key::Unknown);
    CHECK(letter_index(Key::A) == 0);
    CHECK(letter_index(Key::Z) == 25);
    CHECK(letter_index(Key::Num1) == -1);
    CHECK(is_modifier_key(Key::LeftShift));
    CHECK(is_modifier_key(Key::RightSuper));
    CHECK_FALSE(is_modifier_key(Key::Space));
}

TEST_CASE("Modifiers: операции над битовой маской") {
    const Modifiers both = Modifiers::Control | Modifiers::Shift;
    CHECK(has_all(both, Modifiers::Control));
    CHECK(has_all(both, both));
    CHECK_FALSE(has_all(both, Modifiers::Alt));
    CHECK(has_all(Modifiers::None, Modifiers::None));
    CHECK((both & Modifiers::Shift) == Modifiers::Shift);
    Modifiers m = Modifiers::None;
    m |= Modifiers::Alt;
    CHECK(m == Modifiers::Alt);
}

TEST_CASE("index_of: код как индекс таблицы; вне диапазона — kKeyCount") {
    CHECK(index_of(Key::A) == 4);
    CHECK(index_of(static_cast<Key>(255)) == 255);
    CHECK(index_of(static_cast<Key>(256)) == kKeyCount);
    CHECK(index_of(static_cast<Key>(60000)) == kKeyCount);
}

} // TEST_SUITE
