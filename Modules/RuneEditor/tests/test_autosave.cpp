#include <RuneEditor/Analysis.hpp>
#include <RuneEditor/Autosave.hpp>

#include <doctest/doctest.h>

using namespace RuneEditor;
using Runes::Rune;

namespace {
void edit_some(Editor& e) {
    const NodeId t = e.add_node(Rune::Target, {0, 0});
    const NodeId n = e.add_node(Rune::Push, {0, 90}, 2 << 16);
    const NodeId c = e.add_node(Rune::Carve, {150, 0});
    e.connect({t, PortKind::Output, 0}, {c, PortKind::Input, 0});
    e.connect({n, PortKind::Output, 0}, {c, PortKind::Input, 1});
    e.set_entry(c);
    e.remove_node(n);
    e.undo();
    e.move_node(c, {170, 10});
}
} // namespace

TEST_CASE("Autosave: граф восстанавливается из журнала правок") {
    EventLog::MemoryStorage storage;
    Editor e;
    auto save = Autosave::start(storage, e);
    REQUIRE(save.has_value());
    edit_some(e);
    CHECK(save->flush());
    CHECK(save->operations_logged() > 5);

    const auto got = Autosave::recover(storage);
    REQUIRE(got.has_value());
    CHECK(got->graph == e.graph());
    CHECK(got->report.clean());
}

TEST_CASE("Autosave: снимок в начале сохраняет уже имеющийся граф") {
    EventLog::MemoryStorage storage;
    Editor e;
    edit_some(e);
    auto save = Autosave::start(storage, e);
    REQUIRE(save.has_value());
    e.add_node(Rune::Halt, {400, 0});
    save->flush();
    const auto got = Autosave::recover(storage);
    REQUIRE(got.has_value());
    CHECK(got->graph == e.graph());
    CHECK(got->operations == 1);
}

TEST_CASE("Autosave: нумерация узлов после восстановления совпадает с оригиналом") {
    EventLog::MemoryStorage storage;
    Editor e;
    auto save = Autosave::start(storage, e);
    const NodeId a = e.add_node(Rune::Push, {0, 0});
    const NodeId b = e.add_node(Rune::Push, {0, 0});
    e.remove_node(b); // самый старший номер освобождён
    const NodeId c = e.add_node(Rune::Push, {5, 5});
    CHECK(c == b);
    (void)a;
    save->flush();
    const auto got = Autosave::recover(storage);
    REQUIRE(got.has_value());
    CHECK(got->graph == e.graph());
}

TEST_CASE("Autosave: compact переписывает журнал одним снимком") {
    EventLog::MemoryStorage storage;
    Editor e;
    auto save = Autosave::start(storage, e);
    edit_some(e);
    save->flush();
    const auto size_before = storage.size();
    REQUIRE(save->compact());
    CHECK(storage.size() <= size_before);
    e.add_node(Rune::Halt, {9, 9});
    save->flush();
    const auto got = Autosave::recover(storage);
    REQUIRE(got.has_value());
    CHECK(got->graph == e.graph());
    CHECK(got->snapshots == 1);
}

TEST_CASE("Autosave: порча одного блока чинится по чётности") {
    EventLog::MemoryStorage storage;
    Editor e;
    auto save = Autosave::start(storage, e);
    edit_some(e);
    save->flush();
    // портим один блок первой полосы
    const EventLog::Config config{};
    std::byte junk[8];
    for (auto& b : junk) b = std::byte{0xAB};
    storage.write(EventLog::block_offset(config, 0, 0) + 20, junk);
    const auto got = Autosave::recover(storage);
    REQUIRE(got.has_value());
    CHECK(got->graph == e.graph());
    CHECK(got->report.blocks_repaired >= 1);
}

TEST_CASE("Autosave: журнал без снимка — ошибка, а не пустой граф") {
    EventLog::MemoryStorage storage;
    auto w = EventLog::Writer::create(storage);
    REQUIRE(w.has_value());
    Op op;
    w->append(0, op);
    w->flush();
    CHECK_FALSE(Autosave::recover(storage).has_value());
}
