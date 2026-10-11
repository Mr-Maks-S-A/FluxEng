#include <InputSystem/InputSystem.hpp>

#include <doctest/doctest.h>

using namespace InputSystem;

namespace {
KeyInput press(Key k) { return {k, Transition::Press, Modifiers::None}; }
KeyInput release(Key k) { return {k, Transition::Release, Modifiers::None}; }
} // namespace

TEST_SUITE("InputSystem.State") {

TEST_CASE("нажатие: down и pressed в кадре нажатия, потом только down; отпускание: released") {
    InputState s;
    s.begin_frame();
    s.apply(press(Key::Space));
    CHECK(s.down(Key::Space));
    CHECK(s.pressed(Key::Space));
    CHECK_FALSE(s.released(Key::Space));
    s.begin_frame();
    CHECK(s.down(Key::Space));
    CHECK_FALSE(s.pressed(Key::Space));
    s.apply(release(Key::Space));
    CHECK_FALSE(s.down(Key::Space));
    CHECK(s.released(Key::Space));
    s.begin_frame();
    CHECK_FALSE(s.released(Key::Space));
}

TEST_CASE("нажатие и отпускание в одном кадре видны обоими флагами, down — ложь") {
    InputState s;
    s.begin_frame();
    s.apply(press(Key::A));
    s.apply(release(Key::A));
    CHECK(s.pressed(Key::A));
    CHECK(s.released(Key::A));
    CHECK_FALSE(s.down(Key::A));
}

TEST_CASE("автоповтор не меняет состояние и не считается нажатием") {
    InputState s;
    s.begin_frame();
    s.apply(press(Key::D));
    s.begin_frame();
    s.apply(KeyInput{Key::D, Transition::Repeat, Modifiers::None});
    CHECK(s.down(Key::D));
    CHECK_FALSE(s.pressed(Key::D));
}

TEST_CASE("неизвестные и недопустимые коды игнорируются") {
    InputState s;
    s.begin_frame();
    s.apply(press(Key::Unknown));
    s.apply(press(static_cast<Key>(9999)));
    CHECK_FALSE(s.any_key_pressed());
    CHECK_FALSE(s.down(Key::Unknown));
    CHECK_FALSE(s.down(static_cast<Key>(9999)));
    s.apply(MouseButtonInput{static_cast<MouseButton>(200), Transition::Press, Modifiers::None});
    CHECK_FALSE(s.mouse_down(static_cast<MouseButton>(200)));
}

TEST_CASE("any_key_pressed") {
    InputState s;
    s.begin_frame();
    CHECK_FALSE(s.any_key_pressed());
    s.apply(press(Key::F5));
    CHECK(s.any_key_pressed());
    s.begin_frame();
    CHECK_FALSE(s.any_key_pressed());
}

TEST_CASE("мышь: кнопки, курсор и его дельта, колесо за кадр") {
    InputState s;
    s.begin_frame();
    s.apply(CursorInput{100, 50});
    s.begin_frame();
    s.apply(CursorInput{130, 40});
    s.apply(ScrollInput{0, 1});
    s.apply(ScrollInput{0.5, 2});
    s.apply(MouseButtonInput{MouseButton::Left, Transition::Press, Modifiers::None});
    CHECK(s.cursor().x == 130);
    CHECK(s.cursor_delta().x == 30);
    CHECK(s.cursor_delta().y == -10);
    CHECK(s.scroll().y == 3);
    CHECK(s.scroll().x == 0.5);
    CHECK(s.mouse_down(MouseButton::Left));
    CHECK(s.mouse_pressed(MouseButton::Left));
    CHECK_FALSE(s.mouse_down(MouseButton::Right));
    s.begin_frame();
    CHECK(s.scroll().y == 0);
    CHECK(s.cursor_delta().x == 0);
    CHECK(s.mouse_down(MouseButton::Left));
    CHECK_FALSE(s.mouse_pressed(MouseButton::Left));
    s.apply(MouseButtonInput{MouseButton::Left, Transition::Release, Modifiers::None});
    CHECK(s.mouse_released(MouseButton::Left));
}

TEST_CASE("текст: символы по порядку, вместимость ограничена") {
    InputState s;
    s.begin_frame();
    s.apply(CharInput{'h'});
    s.apply(CharInput{0x43F}); // п
    REQUIRE(s.text().size() == 2);
    CHECK(s.text()[1] == 0x43F);
    for (int i = 0; i < 100; ++i) s.apply(CharInput{'x'});
    CHECK(s.text().size() == InputState::text_capacity);
    s.begin_frame();
    CHECK(s.text().empty());
}

TEST_CASE("потеря фокуса отпускает всё зажатое и сообщает об этом через released") {
    InputState s;
    s.begin_frame();
    s.apply(press(Key::W));
    s.apply(MouseButtonInput{MouseButton::Right, Transition::Press, Modifiers::None});
    s.apply(GamepadConnectionInput{0, true});
    s.apply(GamepadButtonInput{0, GamepadButton::A, Transition::Press});
    s.begin_frame();
    s.apply(FocusInput{false});
    CHECK_FALSE(s.focused());
    CHECK_FALSE(s.down(Key::W));
    CHECK(s.released(Key::W));
    CHECK_FALSE(s.mouse_down(MouseButton::Right));
    CHECK(s.mouse_released(MouseButton::Right));
    CHECK_FALSE(s.gamepad_down(0, GamepadButton::A));
    CHECK(s.gamepad_released(0, GamepadButton::A));
    s.apply(FocusInput{true});
    CHECK(s.focused());
}

TEST_CASE("модификаторы выводятся из зажатых клавиш; Caps/Num — из события") {
    InputState s;
    s.begin_frame();
    CHECK(s.modifiers() == Modifiers::None);
    s.apply(press(Key::RightShift));
    s.apply(press(Key::LeftCtrl));
    CHECK(has_all(s.modifiers(), Modifiers::Shift | Modifiers::Control));
    CHECK_FALSE(has_all(s.modifiers(), Modifiers::Alt));
    s.apply(release(Key::RightShift));
    CHECK_FALSE(has_all(s.modifiers(), Modifiers::Shift));
    s.apply(KeyInput{Key::A, Transition::Press, Modifiers::CapsLock});
    CHECK(has_all(s.modifiers(), Modifiers::CapsLock));
}

TEST_CASE("геймпад: подключение, кнопки, оси, значение на начало кадра, отключение") {
    InputState s;
    s.begin_frame();
    CHECK_FALSE(s.gamepad_connected(0));
    s.apply(GamepadConnectionInput{0, true});
    CHECK(s.gamepad_connected(0));
    s.apply(GamepadAxisInput{0, GamepadAxis::LeftX, 0.25f});
    s.begin_frame();
    s.apply(GamepadAxisInput{0, GamepadAxis::LeftX, 0.75f});
    s.apply(GamepadButtonInput{0, GamepadButton::X, Transition::Press});
    CHECK(s.gamepad_axis(0, GamepadAxis::LeftX) == 0.75f);
    CHECK(s.gamepad_axis_previous(0, GamepadAxis::LeftX) == 0.25f);
    CHECK(s.gamepad_pressed(0, GamepadButton::X));
    s.apply(GamepadConnectionInput{0, false});
    CHECK_FALSE(s.gamepad_connected(0));
    CHECK_FALSE(s.gamepad_down(0, GamepadButton::X));
    CHECK(s.gamepad_released(0, GamepadButton::X));
    CHECK(s.gamepad_axis(0, GamepadAxis::LeftX) == 0.0f);
}

TEST_CASE("геймпад: неверный номер игнорируется, запросы безопасны") {
    InputState s;
    s.begin_frame();
    s.apply(GamepadButtonInput{200, GamepadButton::A, Transition::Press});
    s.apply(GamepadAxisInput{9, GamepadAxis::LeftX, 1.0f});
    s.apply(GamepadConnectionInput{77, true});
    CHECK_FALSE(s.gamepad_down(200, GamepadButton::A));
    CHECK(s.gamepad_axis(9, GamepadAxis::LeftX) == 0.0f);
    CHECK_FALSE(s.gamepad_connected(77));
}

TEST_CASE("состояние по умолчанию пусто и безопасно") {
    const InputState s;
    CHECK_FALSE(s.down(Key::A));
    CHECK_FALSE(s.mouse_down(MouseButton::Left));
    CHECK(s.cursor().x == 0);
    CHECK(s.text().empty());
    CHECK(s.focused());
}

} // TEST_SUITE
