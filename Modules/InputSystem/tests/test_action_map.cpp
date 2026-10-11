#include <InputSystem/InputSystem.hpp>

#include <doctest/doctest.h>

#include <cmath>

using namespace InputSystem;

namespace {
KeyInput press(Key k) { return {k, Transition::Press, Modifiers::None}; }
KeyInput release(Key k) { return {k, Transition::Release, Modifiers::None}; }
void connect(InputState& s, std::uint8_t pad = 0) { s.apply(GamepadConnectionInput{pad, true}); }
} // namespace

TEST_SUITE("InputSystem.ActionMap") {

TEST_CASE("ActionId стабилен: FNV-1a 32 бита от имени") {
    CHECK(action_id("jump").value == 0xa73f5c0du);
    CHECK(action_id("jump") == action_id("jump"));
    CHECK_FALSE(action_id("jump") == action_id("Jump"));
}

TEST_CASE("клавиша: down, pressed, released по кадрам") {
    ActionMap actions;
    actions.bind("jump", Key::Space);
    InputState s;
    s.begin_frame();
    CHECK_FALSE(actions.down("jump", s));
    s.apply(press(Key::Space));
    CHECK(actions.down("jump", s));
    CHECK(actions.pressed("jump", s));
    CHECK(actions.value("jump", s) == 1.0f);
    s.begin_frame();
    CHECK(actions.down("jump", s));
    CHECK_FALSE(actions.pressed("jump", s));
    s.apply(release(Key::Space));
    CHECK(actions.released("jump", s));
    CHECK_FALSE(actions.down("jump", s));
}

TEST_CASE("несколько источников одного действия: клавиатура, мышь, геймпад") {
    ActionMap actions;
    actions.bind("fire", Key::F).bind("fire", MouseButton::Left).bind("fire", GamepadButton::X);
    InputState s;
    s.begin_frame();
    s.apply(MouseButtonInput{MouseButton::Left, Transition::Press, Modifiers::None});
    CHECK(actions.pressed("fire", s));
    s.begin_frame();
    s.apply(MouseButtonInput{MouseButton::Left, Transition::Release, Modifiers::None});
    s.begin_frame();
    connect(s, 2);
    s.apply(GamepadButtonInput{2, GamepadButton::X, Transition::Press}); // любой геймпад
    CHECK(actions.pressed("fire", s));
    CHECK(actions.down("fire", s));
}

TEST_CASE("обязательные модификаторы: Ctrl+S срабатывает только с Ctrl") {
    ActionMap actions;
    actions.bind("save", Key::S, Modifiers::Control);
    InputState s;
    s.begin_frame();
    s.apply(press(Key::S));
    CHECK_FALSE(actions.pressed("save", s));
    s.begin_frame();
    s.apply(release(Key::S));
    s.begin_frame();
    s.apply(press(Key::RightCtrl)); // правый Ctrl тоже подходит
    s.apply(press(Key::S));
    CHECK(actions.pressed("save", s));
    CHECK(actions.down("save", s));
}

TEST_CASE("ось из пары клавиш: −1, 0, +1; обе клавиши гасят друг друга") {
    ActionMap actions;
    actions.bind_keys("move_x", Key::A, Key::D);
    InputState s;
    s.begin_frame();
    CHECK(actions.value("move_x", s) == 0.0f);
    s.apply(press(Key::A));
    CHECK(actions.value("move_x", s) == -1.0f);
    s.apply(press(Key::D));
    CHECK(actions.value("move_x", s) == 0.0f);
    s.apply(release(Key::A));
    CHECK(actions.value("move_x", s) == 1.0f);
    CHECK(actions.down("move_x", s));
}

TEST_CASE("мёртвая зона и множитель стика") {
    ActionMap actions;
    actions.bind_axis("look", GamepadAxis::RightX, 1.0f, 0.2f);
    actions.bind_axis("look2", GamepadAxis::RightX, 2.0f, 0.0f);
    InputState s;
    s.begin_frame();
    connect(s);
    s.apply(GamepadAxisInput{0, GamepadAxis::RightX, 0.1f});
    CHECK(actions.value("look", s) == 0.0f);                       // внутри мёртвой зоны
    s.apply(GamepadAxisInput{0, GamepadAxis::RightX, 0.6f});
    CHECK(actions.value("look", s) == doctest::Approx(0.5f));      // (0.6−0.2)/0.8
    CHECK(actions.value("look2", s) == 1.0f);                      // 0.6 × 2 → ограничено единицей
    s.apply(GamepadAxisInput{0, GamepadAxis::RightX, -1.0f});
    CHECK(actions.value("look", s) == doctest::Approx(-1.0f));
}

TEST_CASE("клавиши и стик в одной оси складываются и ограничиваются") {
    ActionMap actions;
    actions.bind_keys("move_x", Key::A, Key::D).bind_axis("move_x", GamepadAxis::LeftX);
    InputState s;
    s.begin_frame();
    connect(s);
    s.apply(press(Key::D));
    s.apply(GamepadAxisInput{0, GamepadAxis::LeftX, 0.9f});
    CHECK(actions.value("move_x", s) == 1.0f);
    s.apply(press(Key::A));
    CHECK(actions.value("move_x", s) == doctest::Approx((0.9f - 0.15f) / 0.85f)); // обе клавиши гасятся, остаётся стик за мёртвой зоной
}

TEST_CASE("аналоговая ось как кнопка: порог 0.5 и фронты по двум кадрам") {
    ActionMap actions;
    actions.bind_axis("brake", GamepadAxis::LeftTrigger, 1.0f, 0.0f);
    InputState s;
    s.begin_frame();
    connect(s);
    s.apply(GamepadAxisInput{0, GamepadAxis::LeftTrigger, 0.2f});
    CHECK_FALSE(actions.down("brake", s));
    s.begin_frame();
    s.apply(GamepadAxisInput{0, GamepadAxis::LeftTrigger, 0.9f});
    CHECK(actions.down("brake", s));
    CHECK(actions.pressed("brake", s));
    s.begin_frame();
    CHECK(actions.down("brake", s));
    CHECK_FALSE(actions.pressed("brake", s));
    s.apply(GamepadAxisInput{0, GamepadAxis::LeftTrigger, 0.1f});
    CHECK(actions.released("brake", s));
}

TEST_CASE("несколько геймпадов: берётся ось с наибольшим отклонением") {
    ActionMap actions;
    actions.bind_axis("move_y", GamepadAxis::LeftY, 1.0f, 0.0f);
    InputState s;
    s.begin_frame();
    connect(s, 0);
    connect(s, 1);
    s.apply(GamepadAxisInput{0, GamepadAxis::LeftY, 0.3f});
    s.apply(GamepadAxisInput{1, GamepadAxis::LeftY, -0.7f});
    CHECK(actions.value("move_y", s) == doctest::Approx(-0.7f));
}

TEST_CASE("vec2: диагональ клавиш не быстрее стика") {
    ActionMap actions;
    actions.bind_keys("x", Key::A, Key::D).bind_keys("y", Key::W, Key::S);
    InputState s;
    s.begin_frame();
    s.apply(press(Key::D));
    s.apply(press(Key::S));
    const Vec2f v = actions.vec2("x", "y", s);
    CHECK(v.x == doctest::Approx(0.70710678f));
    CHECK(v.y == doctest::Approx(0.70710678f));
    CHECK(std::sqrt(v.x * v.x + v.y * v.y) == doctest::Approx(1.0f));
}

TEST_CASE("переназначение: unbind и новая привязка; неизвестное действие безопасно") {
    ActionMap actions;
    actions.bind("jump", Key::Space);
    CHECK(actions.has("jump"));
    CHECK(actions.unbind("jump"));
    CHECK_FALSE(actions.unbind("jump"));
    CHECK_FALSE(actions.has("jump"));
    actions.bind("jump", Key::J);
    InputState s;
    s.begin_frame();
    s.apply(press(Key::Space));
    CHECK_FALSE(actions.down("jump", s));
    s.apply(press(Key::J));
    CHECK(actions.down("jump", s));
    CHECK_FALSE(actions.down("nonexistent", s));
    CHECK(actions.value("nonexistent", s) == 0.0f);
    CHECK(actions.bindings(action_id("nonexistent")).empty());
}

TEST_CASE("имена действий: порядок создания, проверка символов") {
    ActionMap actions;
    actions.bind("b", Key::B).bind("a", Key::A).bind("b", Key::C);
    CHECK(actions.names() == std::vector<std::string_view>{"b", "a"});
    CHECK(actions.size() == 2);
    CHECK(actions.bindings(action_id("b")).size() == 2);
    CHECK_THROWS_AS(actions.bind("", Key::A), std::invalid_argument);
    CHECK_THROWS_AS(actions.bind("has space", Key::A), std::invalid_argument);
    CHECK_THROWS_AS(actions.bind("a:b", Key::A), std::invalid_argument);
}

TEST_CASE("текст: формат и круговой обход, включая все виды привязок") {
    ActionMap actions;
    actions.bind("jump", Key::Space).bind("jump", GamepadButton::A);
    actions.bind_keys("move_x", Key::A, Key::D).bind_axis("move_x", GamepadAxis::LeftX);
    actions.bind_axis("look_x", GamepadAxis::RightX, 2.0f, 0.2f);
    actions.bind("save", Key::S, Modifiers::Control | Modifiers::Shift);
    actions.bind("fire", MouseButton::Left, Modifiers::Alt);
    const std::string text = actions.to_text();
    CHECK(text ==
          "jump: Key:Space Pad:A\n"
          "move_x: Keys:A,D Axis:LeftX\n"
          "look_x: Axis:RightX*2~0.2\n"
          "save: Ctrl+Shift+Key:S\n"
          "fire: Alt+Mouse:Left\n");
    const auto parsed = ActionMap::from_text(text);
    REQUIRE_MESSAGE(parsed.has_value(), parsed.error());
    CHECK(*parsed == actions);
    CHECK(parsed->to_text() == text);
}

TEST_CASE("текст: комментарии, пустые строки и произвольные пробелы") {
    const auto parsed = ActionMap::from_text("# настройки\n\n  jump :  Key:Space   Pad:A  # прыжок\n\tfire:Mouse:Left\n");
    REQUIRE_MESSAGE(parsed.has_value(), parsed.error());
    CHECK(parsed->bindings(action_id("jump")).size() == 2);
    CHECK(parsed->has("fire"));
}

TEST_CASE("текст: ошибки называют номер строки") {
    const auto expect_error = [](std::string_view text, std::string_view fragment) {
        CAPTURE(text);
        const auto r = ActionMap::from_text(text);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find(fragment) != std::string::npos);
    };
    expect_error("jump Key:Space", "line 1");
    expect_error("\n\njump: Key:Spacebar", "line 3");
    expect_error("jump: Key:Space\njump: Key:J", "twice");
    expect_error("jump: Foo:Bar", "unknown source");
    expect_error("jump: Key", "Source:Name");
    expect_error("jump: Hyper+Key:A", "modifier");
    expect_error("look: Axis:RightX*abc", "scale");
    expect_error("look: Axis:RightX~1.5", "dead zone");
    expect_error("look: Axis:Nope", "axis");
    expect_error("m: Keys:A", "two keys");
    expect_error("bad name: Key:A", "bad action name");
    expect_error("m: Pad:Z", "gamepad button");
    expect_error("m: Mouse:Wheel", "mouse button");
}

TEST_CASE("пустой текст — пустая карта; переназначение через текст заменяет привязки") {
    CHECK(ActionMap::from_text("")->size() == 0);
    ActionMap defaults;
    defaults.bind("jump", Key::Space);
    const auto user = ActionMap::from_text("jump: Key:J\n");
    REQUIRE(user.has_value());
    defaults.unbind("jump");
    for (const Binding& b : user->bindings(action_id("jump"))) {
        if (b.source == Binding::Source::Key) defaults.bind("jump", static_cast<Key>(b.code), b.required);
    }
    InputState s;
    s.begin_frame();
    s.apply(press(Key::J));
    CHECK(defaults.down("jump", s));
}

} // TEST_SUITE
