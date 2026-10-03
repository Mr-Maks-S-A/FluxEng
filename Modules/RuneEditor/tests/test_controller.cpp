#include <RuneEditor/Controller.hpp>

#include <doctest/doctest.h>

using namespace RuneEditor;
using Runes::Rune;

namespace {
struct Scene {
    Editor editor;
    Controller ctl{editor};
    NodeId target, carve;
    Scene() {
        target = editor.add_node(Rune::Target, {0, 0});
        carve = editor.add_node(Rune::Carve, {200, 0});
    }
    Vec2 screen_of(const PortRef& p) { return ctl.camera.to_screen(*port_position(editor.graph(), p)); }
    void drag(Button b, Vec2 from, Vec2 to) {
        ctl.press(b, from);
        ctl.move((from + to) * 0.5f);
        ctl.move(to);
        ctl.release(b, to);
    }
};
} // namespace

TEST_CASE("Controller: перетаскивание узла — одна правка истории") {
    Scene s;
    s.drag(Button::Left, {0, 0}, {40, 25});
    CHECK(s.editor.graph().find(s.target)->x == 40.0f);
    CHECK(s.editor.graph().find(s.target)->y == 25.0f);
    CHECK(s.editor.selection().contains(s.target));
    CHECK(s.editor.undo());
    CHECK(s.editor.graph().find(s.target)->x == 0.0f);
}

TEST_CASE("Controller: ребро тянется от порта к порту") {
    Scene s;
    s.drag(Button::Left, s.screen_of({s.target, PortKind::Output, 0}), s.screen_of({s.carve, PortKind::Input, 0}));
    CHECK(s.editor.graph().find(s.carve)->inputs[0] == s.target);
}

TEST_CASE("Controller: отпустить в пустоте — ничего не создаётся") {
    Scene s;
    const Graph before = s.editor.graph();
    s.drag(Button::Left, s.screen_of({s.target, PortKind::Output, 0}), {500, 500});
    CHECK(s.editor.graph() == before);
}

TEST_CASE("Controller: поднятое ребро переносится, в пустоте — удаляется, Esc возвращает") {
    Scene s;
    const NodeId two = s.editor.add_node(Rune::Push, {0, 90}, 2 << 16);
    const NodeId sum = s.editor.add_node(Rune::Add, {200, 180});
    s.editor.connect({two, PortKind::Output, 0}, {s.carve, PortKind::Input, 1});
    // перенос
    s.drag(Button::Left, s.screen_of({s.carve, PortKind::Input, 1}), s.screen_of({sum, PortKind::Input, 0}));
    CHECK(s.editor.graph().find(sum)->inputs[0] == two);
    CHECK(s.editor.graph().find(s.carve)->inputs[1] == Runes::no_node);
    s.editor.undo();
    CHECK(s.editor.graph().find(s.carve)->inputs[1] == two); // перенос — один шаг
    // удаление
    s.drag(Button::Left, s.screen_of({s.carve, PortKind::Input, 1}), {900, 900});
    CHECK(s.editor.graph().find(s.carve)->inputs[1] == Runes::no_node);
    s.editor.undo();
    // отмена
    s.ctl.press(Button::Left, s.screen_of({s.carve, PortKind::Input, 1}));
    CHECK(s.editor.graph().find(s.carve)->inputs[1] == Runes::no_node);
    s.ctl.cancel();
    CHECK(s.editor.graph().find(s.carve)->inputs[1] == two);
}

TEST_CASE("Controller: рамка выделяет узлы") {
    Scene s;
    s.drag(Button::Left, {-100, -100}, {100, 100});
    CHECK(s.editor.selection() == std::set<NodeId>{s.target});
    s.drag(Button::Left, {-100, -100}, {400, 100});
    CHECK(s.editor.selection().size() == 2);
}

TEST_CASE("Controller: масштаб держит точку под курсором, правая кнопка сдвигает") {
    Scene s;
    const Vec2 cursor{120, 80};
    const Vec2 before = s.ctl.camera.to_world(cursor);
    s.ctl.wheel(3.0f, cursor);
    CHECK(s.ctl.camera.zoom > 1.0f);
    const Vec2 after = s.ctl.camera.to_world(cursor);
    CHECK(doctest::Approx(after.x) == before.x);
    CHECK(doctest::Approx(after.y) == before.y);
    const Vec2 pan = s.ctl.camera.pan;
    s.drag(Button::Right, {300, 300}, {250, 300});
    CHECK(s.ctl.camera.pan.x > pan.x);
}

TEST_CASE("Controller: палитра ставит узел под курсор") {
    Scene s;
    s.ctl.camera.zoom = 2.0f;
    const NodeId id = s.ctl.place(Rune::Push, {200, 100}, 1 << 16);
    CHECK(s.editor.graph().find(id)->x == 100.0f);
    CHECK(s.editor.graph().find(id)->y == 50.0f);
    CHECK(s.editor.selection() == std::set<NodeId>{id});
}
