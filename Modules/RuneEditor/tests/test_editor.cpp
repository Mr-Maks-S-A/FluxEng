#include <RuneEditor/Analysis.hpp>
#include <RuneEditor/Autosave.hpp>
#include <RuneEditor/Controller.hpp>
#include <RuneEditor/Editor.hpp>
#include <RuneEditor/Layout.hpp>

#include <doctest/doctest.h>

using namespace RuneEditor;
using Runes::Rune;

namespace {
/// Рабочий граф: CARVE(TARGET, PUSH 2) → HALT. Строится операциями редактора.
struct Built {
    NodeId target, two, carve, halt;
};
Built build(Editor& e) {
    Built b{};
    b.target = e.add_node(Rune::Target, {0, 0});
    b.two = e.add_node(Rune::Push, {0, 90}, 2 << 16);
    b.carve = e.add_node(Rune::Carve, {150, 0});
    b.halt = e.add_node(Rune::Halt, {300, 0});
    e.begin();
    e.connect({b.target, PortKind::Output, 0}, {b.carve, PortKind::Input, 0});
    e.connect({b.two, PortKind::Output, 0}, {b.carve, PortKind::Input, 1});
    e.connect({b.carve, PortKind::Next, 0}, {b.halt, PortKind::Input, 0});
    e.set_entry(b.carve);
    e.end();
    return b;
}
} // namespace

TEST_CASE("Editor: граф из правок компилируется") {
    Editor e;
    const Built b = build(e);
    const Analysis a = analyze(e.graph());
    CHECK(a.ok());
    CHECK(a.problems.empty());
    CHECK(a.source.size() == a.program->code.size());
    CHECK(a.first_rune_of(b.halt).has_value());
}

TEST_CASE("Editor: недопустимые правки отклоняются и не трогают историю") {
    Editor e;
    const Built b = build(e);
    // ширина: вектор в число
    CHECK_FALSE(e.connect({b.target, PortKind::Output, 0}, {b.carve, PortKind::Input, 1}));
    // цикл данных: ADD сам в себя
    const NodeId add = e.add_node(Rune::Add, {0, 0});
    const NodeId add2 = e.add_node(Rune::Add, {0, 0});
    CHECK(e.connect({add, PortKind::Output, 0}, {add2, PortKind::Input, 0}));
    CHECK_FALSE(e.connect({add2, PortKind::Output, 0}, {add, PortKind::Input, 0}));
    CHECK_FALSE(e.connect({add, PortKind::Output, 0}, {add, PortKind::Input, 0}));
    // оператор нельзя подключить как значение, а значение — в цепочку управления
    CHECK_FALSE(e.connect({b.carve, PortKind::Output, 0}, {add, PortKind::Input, 1}));
    CHECK_FALSE(e.connect({b.carve, PortKind::Next, 0}, {b.two, PortKind::Input, 0}));
    CHECK_FALSE(e.set_value(b.target, 5));
}

TEST_CASE("Editor: undo и redo возвращают граф точно") {
    Editor e;
    const Built b = build(e);
    const Graph before = e.graph();
    CHECK(e.remove_node(b.two));
    CHECK_FALSE(e.graph() == before);
    CHECK(e.undo());
    CHECK(e.graph() == before);
    CHECK(e.redo());
    CHECK(e.graph().find(b.two) == nullptr);
    // новая правка обрезает «будущее»
    CHECK(e.undo());
    e.add_node(Rune::Push, {1, 1});
    CHECK_FALSE(e.can_redo());
}

TEST_CASE("Editor: группа правок — один шаг истории") {
    Editor e;
    const Graph empty = e.graph();
    e.begin();
    e.add_node(Rune::Push, {0, 0});
    e.add_node(Rune::Push, {1, 1});
    e.end();
    CHECK(e.graph().nodes().size() == 2);
    CHECK(e.undo());
    CHECK(e.graph() == empty);
    CHECK_FALSE(e.can_undo());
}

TEST_CASE("Editor: удаление узла не сдвигает номера входов") {
    Editor e;
    const Built b = build(e);
    e.remove_node(b.target);
    const GraphNode& carve = *e.graph().find(b.carve);
    REQUIRE(carve.inputs.size() == 2);
    CHECK(carve.inputs[0] == Runes::no_node);
    CHECK(carve.inputs[1] == b.two);
    const Analysis a = analyze(e.graph());
    CHECK_FALSE(a.ok());
    REQUIRE(a.worst(b.carve) != nullptr);
    CHECK(a.worst(b.carve)->code == Runes::Code::InputNotConnected);
}

TEST_CASE("Analysis: предупреждения о недостижимом и неиспользуемом") {
    Editor e;
    build(e);
    const NodeId lone = e.add_node(Rune::Halt, {0, 200});
    const NodeId unused = e.add_node(Rune::Push, {0, 300});
    const Analysis a = analyze(e.graph());
    CHECK(a.ok());
    REQUIRE(a.worst(lone) != nullptr);
    CHECK(a.worst(lone)->code == Runes::Code::UnreachableNode);
    CHECK_FALSE(a.worst(lone)->error);
    REQUIRE(a.worst(unused) != nullptr);
    CHECK(a.worst(unused)->code == Runes::Code::UnusedValue);
}

TEST_CASE("Analysis: стоимость собранного графа") {
    Editor e;
    build(e);
    const Analysis a = analyze(e.graph());
    CHECK(a.cost.effects == 1);
    CHECK(a.cost.per_pass.raw() > 0);
}

TEST_CASE("Editor: перетаскивание не шумит, фиксация — одна группа") {
    Editor e;
    const Built b = build(e);
    int ops = 0;
    e.set_observer([&](const Op&) { ++ops; });
    const Graph before = e.graph();
    e.preview_move(b.carve, {10, 10});
    e.preview_move(b.carve, {50, 60});
    CHECK(ops == 0);
    CHECK(e.graph().find(b.carve)->x == 50.0f);
    e.commit_preview();
    CHECK(e.graph().find(b.carve)->y == 60.0f);
    CHECK(ops == 3); // Begin, MoveNode, End
    CHECK(e.undo());
    CHECK(e.graph() == before);
}

TEST_CASE("Geometry: порты и выбор под курсором") {
    Editor e;
    const Built b = build(e);
    const auto out = port_position(e.graph(), {b.target, PortKind::Output, 0});
    REQUIRE(out.has_value());
    CHECK(pick(e.graph(), *out).kind == Pick::Kind::Port);
    CHECK(pick(e.graph(), {0, 0}).kind == Pick::Kind::Node);
    CHECK(pick(e.graph(), {-500, -500}).kind == Pick::Kind::None);
    // середина ребра TARGET → CARVE
    const auto ends = edge_endpoints(e.graph(), {b.target, PortKind::Output, b.carve, 0});
    REQUIRE(ends.has_value());
    const auto line = edge_polyline(ends->first, ends->second);
    const Pick on_edge = pick(e.graph(), line[line.size() / 2]);
    CHECK(on_edge.kind == Pick::Kind::Edge);
}

TEST_CASE("Layout: колонки следуют за потоком управления") {
    Editor e;
    const Built b = build(e);
    Graph g = e.graph();
    auto_layout(g);
    CHECK(g.find(b.carve)->x < g.find(b.halt)->x);
    CHECK(g.find(b.target)->x < g.find(b.carve)->x);
    CHECK(g.find(b.two)->x < g.find(b.carve)->x);
    CHECK(g.find(b.target)->y != g.find(b.two)->y); // не накладываются
    CHECK(analyze(g).ok());
}
