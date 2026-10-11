#include <InputSystem/InputSystem.hpp>

#include <doctest/doctest.h>

#include <cstring>

using namespace InputSystem;

namespace {
KeyInput press(Key k) { return {k, Transition::Press, Modifiers::None}; }

ActionMap make_actions() {
    ActionMap a;
    a.bind("jump", Key::Space).bind("jump", GamepadButton::A);
    a.bind("cast", MouseButton::Left);
    a.bind_keys("move_x", Key::A, Key::D).bind_axis("move_x", GamepadAxis::LeftX);
    a.bind_keys("move_y", Key::W, Key::S).bind_axis("move_y", GamepadAxis::LeftY);
    return a;
}

CommandLayout make_layout() {
    CommandLayout layout;
    layout.button("jump").button("cast").axis("move_x").axis("move_y");
    return layout;
}
} // namespace

TEST_SUITE("InputSystem.Command") {

TEST_CASE("команда — 16 байт без padding и без float: годится как команда lockstep") {
    static_assert(sizeof(InputCommand) == 16);
    static_assert(std::has_unique_object_representations_v<InputCommand>);
    static_assert(std::is_trivially_copyable_v<InputCommand>);
    const InputCommand zero;
    CHECK(zero.buttons == 0);
    CHECK(zero.axes[3] == 0);
}

TEST_CASE("снимок: кнопки — по битам в порядке раскладки, оси — квантованы в int16") {
    const ActionMap actions = make_actions();
    const CommandLayout layout = make_layout();
    InputState s;
    s.begin_frame();
    s.apply(press(Key::Space));
    s.apply(press(Key::D));
    s.apply(press(Key::W));
    const InputCommand c = sample_command(actions, s, layout);
    CHECK(c.down(0));            // jump
    CHECK_FALSE(c.down(1));      // cast
    CHECK(c.buttons == 1u);
    CHECK(c.axes[0] == 32767);   // move_x = +1
    CHECK(c.axes[1] == -32767);  // move_y: W = −1
    CHECK(c.axis(0) == doctest::Approx(1.0f));
    CHECK(c.axes[2] == 0);
    CHECK_FALSE(c.down(31));
    CHECK_FALSE(c.down(99)); // за пределами — безопасно
}

TEST_CASE("квантование: половина стика, знак и ограничение") {
    ActionMap actions;
    actions.bind_axis("x", GamepadAxis::LeftX, 1.0f, 0.0f).bind_axis("y", GamepadAxis::LeftY, 3.0f, 0.0f);
    CommandLayout layout;
    layout.axis("x").axis("y");
    InputState s;
    s.begin_frame();
    s.apply(GamepadConnectionInput{0, true});
    s.apply(GamepadAxisInput{0, GamepadAxis::LeftX, 0.5f});
    s.apply(GamepadAxisInput{0, GamepadAxis::LeftY, -0.9f}); // × 3 → ограничено −1
    const InputCommand c = sample_command(actions, s, layout);
    CHECK(c.axes[0] == 16384);
    CHECK(c.axes[1] == -32767);
}

TEST_CASE("один и тот же ввод — побайтно одинаковые команды (на любом узле)") {
    const ActionMap actions = make_actions();
    const CommandLayout layout = make_layout();
    InputCommand first, second;
    for (InputCommand* out : {&first, &second}) {
        InputState s;
        s.begin_frame();
        s.apply(press(Key::A));
        s.apply(MouseButtonInput{MouseButton::Left, Transition::Press, Modifiers::None});
        *out = sample_command(actions, s, layout);
    }
    CHECK(first == second);
    CHECK(std::memcmp(&first, &second, sizeof(InputCommand)) == 0);
}

TEST_CASE("клавиша и геймпад дают одну и ту же команду — симуляции всё равно, откуда ввод") {
    const ActionMap actions = make_actions();
    const CommandLayout layout = make_layout();
    InputState keyboard;
    keyboard.begin_frame();
    keyboard.apply(press(Key::Space));
    InputState pad;
    pad.begin_frame();
    pad.apply(GamepadConnectionInput{0, true});
    pad.apply(GamepadButtonInput{0, GamepadButton::A, Transition::Press});
    CHECK(sample_command(actions, keyboard, layout) == sample_command(actions, pad, layout));
}

TEST_CASE("фронты нажатия и отпускания восстанавливаются из двух подряд идущих команд") {
    InputCommand previous, current;
    current.buttons = 0b101;
    CHECK(button_pressed(previous, current, 0));
    CHECK_FALSE(button_pressed(previous, current, 1));
    CHECK(button_pressed(previous, current, 2));
    previous = current;
    current.buttons = 0b100;
    CHECK(button_released(previous, current, 0));
    CHECK_FALSE(button_pressed(previous, current, 2)); // держится — не новое нажатие
}

TEST_CASE("раскладка: не больше 32 кнопок и 6 осей; неизвестные действия дают ноль") {
    CommandLayout layout;
    for (int i = 0; i < 32; ++i) layout.button("b" + std::to_string(i));
    CHECK_THROWS_AS(layout.button("b32"), std::length_error);
    for (int i = 0; i < 6; ++i) layout.axis("a" + std::to_string(i));
    CHECK_THROWS_AS(layout.axis("a6"), std::length_error);
    InputState s;
    s.begin_frame();
    const InputCommand c = sample_command(ActionMap{}, s, layout);
    CHECK(c == InputCommand{});
}

} // TEST_SUITE
