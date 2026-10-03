#include <Core/Actions.hpp>

#include <GLFW/glfw3.h>

#include <doctest/doctest.h>

using namespace Core;
using WindowSystem::InputState;
using WindowSystem::action_press;
using WindowSystem::action_release;

namespace {
struct Rig {
    ActionMap map;
    InputState input;
    ActionId jump = map.declare("jump", "Прыжок");
    ActionId forward = map.declare("move_forward");
    ActionId back = map.declare("move_back");
    Rig() {
        map.bind(jump, Binding::key(GLFW_KEY_SPACE));
        map.bind(forward, Binding::key(GLFW_KEY_W)).bind(forward, Binding::key(GLFW_KEY_UP));
        map.bind(back, Binding::key(GLFW_KEY_S));
    }
};
} // namespace

TEST_CASE("действие срабатывает от любой из своих привязок: down, pressed, released") {
    Rig r;
    CHECK_FALSE(r.map.down(r.input, r.forward));
    r.input.on_key(GLFW_KEY_UP, action_press); // вторая клавиша действия
    CHECK(r.map.down(r.input, r.forward));
    CHECK(r.map.pressed(r.input, r.forward));
    r.input.begin_frame();
    CHECK(r.map.down(r.input, r.forward));
    CHECK_FALSE(r.map.pressed(r.input, r.forward)); // «нажали» — только в кадре нажатия
    r.input.on_key(GLFW_KEY_UP, action_release);
    CHECK(r.map.released(r.input, r.forward));
    CHECK_FALSE(r.map.down(r.input, r.forward));
}

TEST_CASE("мышь как привязка") {
    Rig r;
    const ActionId fire = r.map.declare("fire");
    r.map.bind(fire, Binding::mouse(GLFW_MOUSE_BUTTON_LEFT));
    r.input.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, action_press);
    CHECK(r.map.pressed(r.input, fire));
    CHECK_FALSE(r.map.pressed(r.input, r.jump));
}

TEST_CASE("ось из двух действий: +1, −1, ноль при обоих") {
    Rig r;
    CHECK(r.map.axis(r.input, r.forward, r.back) == 0.0f);
    r.input.on_key(GLFW_KEY_W, action_press);
    CHECK(r.map.axis(r.input, r.forward, r.back) == 1.0f);
    r.input.on_key(GLFW_KEY_S, action_press);
    CHECK(r.map.axis(r.input, r.forward, r.back) == 0.0f);
    r.input.on_key(GLFW_KEY_W, action_release);
    CHECK(r.map.axis(r.input, r.forward, r.back) == -1.0f);
}

TEST_CASE("declare идемпотентен, find по имени, неизвестный номер безопасен") {
    Rig r;
    CHECK(r.map.declare("jump") == r.jump);
    CHECK(r.map.size() == 3);
    CHECK(r.map.find("move_back") == r.back);
    CHECK_FALSE(r.map.find("fly").has_value());
    CHECK(r.map.description(r.jump) == "Прыжок");
    CHECK_FALSE(r.map.down(r.input, ActionId{})); // «пустое» действие не срабатывает и не падает
}

TEST_CASE("перепривязка: bind без дублей, unbind, unbind_all, конфликты") {
    Rig r;
    r.map.bind(r.jump, Binding::key(GLFW_KEY_SPACE)); // повтор игнорируется
    CHECK(r.map.bindings(r.jump).size() == 1);
    CHECK(r.map.unbind(r.jump, Binding::key(GLFW_KEY_SPACE)));
    CHECK_FALSE(r.map.unbind(r.jump, Binding::key(GLFW_KEY_SPACE)));
    r.map.bind(r.jump, Binding::key(GLFW_KEY_W)); // та же клавиша, что у «вперёд»
    const auto clash = r.map.conflicts(r.jump);
    REQUIRE(clash.size() == 1);
    CHECK(clash[0] == r.forward);
    r.map.unbind_all(r.jump);
    CHECK(r.map.bindings(r.jump).empty());
    CHECK(r.map.conflicts(r.jump).empty());
}

TEST_CASE("имена привязок туда и обратно, регистр не важен") {
    for (const int key : {GLFW_KEY_A, GLFW_KEY_Z, GLFW_KEY_0, GLFW_KEY_9, GLFW_KEY_F1, GLFW_KEY_F12, GLFW_KEY_SPACE, GLFW_KEY_LEFT_SHIFT, GLFW_KEY_UP}) {
        const Binding b = Binding::key(key);
        const auto parsed = parse_binding(binding_name(b));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == b);
    }
    CHECK(binding_name(Binding::mouse(GLFW_MOUSE_BUTTON_RIGHT)) == "MOUSE_RIGHT");
    CHECK(parse_binding("mouse_left") == Binding::mouse(GLFW_MOUSE_BUTTON_LEFT));
    CHECK(parse_binding(" f5 ") == Binding::key(GLFW_KEY_F5));
    CHECK(binding_name(Binding::key(GLFW_KEY_W)) == "W");
    CHECK_FALSE(parse_binding("F13").has_value());
    CHECK_FALSE(parse_binding("banana").has_value());
    CHECK(binding_name(Binding::key(999)) == "KEY_999");
}

TEST_CASE("apply: файл игрока переопределяет только упомянутые действия") {
    Rig r;
    const auto ok = r.map.apply("# мой файл\njump = F, MOUSE_RIGHT   # левой рукой\nmove_back =\n");
    REQUIRE(ok.has_value());
    CHECK(r.map.bindings(r.jump).size() == 2);
    CHECK(r.map.bindings(r.back).empty());               // пустая правая часть — без привязок
    CHECK(r.map.bindings(r.forward).size() == 2);        // не упомянуто — не тронуто
    r.input.on_mouse_button(GLFW_MOUSE_BUTTON_RIGHT, action_press);
    CHECK(r.map.pressed(r.input, r.jump));
}

TEST_CASE("apply: ошибки с номером строки, ничего не применяется") {
    Rig r;
    const auto before = r.map.serialize();
    const auto bad_action = r.map.apply("jump = F\nfly = X\n");
    REQUIRE_FALSE(bad_action.has_value());
    CHECK(bad_action.error().find("строка 2") != std::string::npos);
    CHECK(bad_action.error().find("fly") != std::string::npos);
    const auto bad_key = r.map.apply("jump = banana\n");
    REQUIRE_FALSE(bad_key.has_value());
    CHECK(bad_key.error().find("banana") != std::string::npos);
    CHECK_FALSE(r.map.apply("just text\n").has_value());
    CHECK(r.map.serialize() == before); // всё или ничего
}

TEST_CASE("serialize даёт файл, который apply читает обратно") {
    Rig r;
    const std::string text = r.map.serialize();
    CHECK(text.find("jump = SPACE") != std::string::npos);
    CHECK(text.find("move_forward = W, UP") != std::string::npos);
    CHECK(text.find("# Прыжок") != std::string::npos);
    Rig other;
    other.map.unbind_all(other.jump);
    other.map.unbind_all(other.forward);
    REQUIRE(other.map.apply(text).has_value());
    CHECK(other.map.serialize() == text);
}
