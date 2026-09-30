#include <WindowSystem/Input.hpp>

#include <doctest/doctest.h>

using WindowSystem::action_press;
using WindowSystem::action_release;
using WindowSystem::action_repeat;
using WindowSystem::InputState;

namespace {
constexpr int key_a = 65;      // GLFW_KEY_A
constexpr int key_space = 32;  // GLFW_KEY_SPACE
constexpr int mouse_left = 0;  // GLFW_MOUSE_BUTTON_LEFT
constexpr int key_unknown = -1;
} // namespace

TEST_SUITE("WindowSystem.Input") {

TEST_CASE("InputState{} — ничего не нажато (ZII)") {
    const InputState input{};
    CHECK_FALSE(input.down(key_a));
    CHECK_FALSE(input.pressed(key_a));
    CHECK(input.cursor().x == 0.0);
    CHECK(input.scroll().y == 0.0);
    CHECK(input.text().empty());
}

TEST_CASE("нажатие: pressed один кадр, down до отпускания") {
    InputState input;
    input.begin_frame();
    input.on_key(key_a, action_press);
    CHECK(input.down(key_a));
    CHECK(input.pressed(key_a));

    input.begin_frame();
    CHECK(input.down(key_a));
    CHECK_FALSE(input.pressed(key_a));

    input.on_key(key_a, action_repeat); // автоповтор не считается новым нажатием
    CHECK_FALSE(input.pressed(key_a));

    input.begin_frame();
    input.on_key(key_a, action_release);
    CHECK_FALSE(input.down(key_a));
    CHECK(input.released(key_a));
    input.begin_frame();
    CHECK_FALSE(input.released(key_a));
}

TEST_CASE("нажатие и отпускание внутри одного кадра не теряется") {
    InputState input;
    input.begin_frame();
    input.on_key(key_space, action_press);
    input.on_key(key_space, action_release);
    CHECK(input.pressed(key_space));
    CHECK(input.released(key_space));
    CHECK_FALSE(input.down(key_space));
}

TEST_CASE("неизвестные и неверные коды игнорируются") {
    InputState input;
    input.on_key(key_unknown, action_press);
    input.on_key(100000, action_press);
    input.on_mouse_button(99, action_press);
    CHECK_FALSE(input.down(key_unknown));
    CHECK_FALSE(input.mouse_down(99));
}

TEST_CASE("мышь: кнопки, курсор, смещение за кадр, колесо суммируется") {
    InputState input;
    input.on_cursor(100.0, 50.0);
    input.begin_frame();
    input.on_cursor(110.0, 45.0);
    input.on_mouse_button(mouse_left, action_press);
    input.on_scroll(0.0, 1.0);
    input.on_scroll(0.0, 2.0);

    CHECK(input.mouse_pressed(mouse_left));
    CHECK(input.mouse_down(mouse_left));
    CHECK(input.cursor_delta().x == 10.0);
    CHECK(input.cursor_delta().y == -5.0);
    CHECK(input.scroll().y == 3.0);

    input.begin_frame();
    CHECK(input.scroll().y == 0.0);
    CHECK(input.cursor_delta().x == 0.0);
    CHECK(input.mouse_down(mouse_left));
}

TEST_CASE("текст за кадр, лишнее отбрасывается") {
    InputState input;
    input.begin_frame();
    for (std::uint32_t c = 0; c < 40; ++c) input.on_char('a' + c % 26);
    CHECK(input.text().size() == InputState::text_capacity);
    CHECK(input.text()[0] == 'a');
    input.begin_frame();
    CHECK(input.text().empty());
}

TEST_CASE("потеря фокуса отпускает всё") {
    InputState input;
    input.begin_frame();
    input.on_key(key_a, action_press);
    input.on_mouse_button(mouse_left, action_press);
    input.begin_frame();
    input.on_focus_lost();
    CHECK_FALSE(input.down(key_a));
    CHECK(input.released(key_a));
    CHECK_FALSE(input.mouse_down(mouse_left));
}

}
