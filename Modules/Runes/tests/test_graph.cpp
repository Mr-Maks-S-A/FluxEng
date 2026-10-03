#include <Runes/Runes.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace Runes;
using Math::Fixed;
using Math::Mana;
using Math::literals::operator""_fx;

namespace {

/// Граф «вырезать шар радиуса 2 в точке прицела».
Graph carve_graph() {
    Graph g;
    const NodeId target = g.add(Rune::Target, 0, 40, 20), radius = g.add(Rune::Push, (2_fx).raw, 40, 80);
    const NodeId carve = g.add(Rune::Carve, 0, 200, 40), halt = g.add(Rune::Halt, 0, 360, 40);
    g.set_input(carve, 0, target), g.set_input(carve, 1, radius);
    g.set_next(carve, halt);
    g.entry = carve;
    return g;
}

std::vector<Instruction> strip_halt(std::vector<Instruction> code) {
    while (!code.empty() && code.back().rune == Rune::Halt) code.pop_back();
    return code;
}
bool same_code(const std::vector<Instruction>& a, const std::vector<Instruction>& b) {
    return strip_halt(a).size() == strip_halt(b).size() && std::ranges::equal(strip_halt(a), strip_halt(b), [](const Instruction& x, const Instruction& y) {
               return x.rune == y.rune && x.operand == y.operand;
           });
}

} // namespace

TEST_CASE("граф компилируется в тот же байт-код, что и текст") {
    const auto from_graph = compile(carve_graph());
    const auto from_text = parse_program("TARGET\nPUSH 2\nCARVE\nHALT\n");
    REQUIRE(from_graph.has_value());
    REQUIRE(from_text.has_value());
    CHECK(from_graph->code.size() == from_text->code.size());
    CHECK(same_code(from_graph->code, from_text->code));
}

TEST_CASE("выражения: ADD и MUL, общий узел кормит двух потребителей") {
    Graph g;
    const NodeId one = g.add(Rune::Push, (1_fx).raw), two = g.add(Rune::Push, (2_fx).raw);
    const NodeId sum = g.add(Rune::Add), half = g.add(Rune::Push, (0.5_fx).raw), mul = g.add(Rune::Mul);
    const NodeId caster = g.add(Rune::Caster), raise = g.add(Rune::Raise);
    g.set_input(sum, 0, one), g.set_input(sum, 1, two);
    g.set_input(mul, 0, sum), g.set_input(mul, 1, half);
    g.set_input(raise, 0, caster), g.set_input(raise, 1, mul);
    g.entry = raise;
    const auto p = compile(g);
    REQUIRE(p.has_value());
    const auto text = parse_program("CASTER\nPUSH 1\nPUSH 2\nADD\nPUSH 0.5\nMUL\nRAISE\n");
    CHECK(same_code(p->code, text->code));
}

TEST_CASE("цикл: рёбра назад, JMP_IF с веткой") {
    Graph g; // loop: PUSH 1; JMP_IF loop
    const NodeId one = g.add(Rune::Push, (1_fx).raw), jump = g.add(Rune::JmpIf);
    g.set_input(jump, 0, one);
    g.set_branch(jump, jump);
    g.entry = jump;
    const auto p = compile(g);
    REQUIRE(p.has_value());
    CHECK(same_code(p->code, parse_program("loop:\nPUSH 1\nJMP_IF loop\n")->code));
}

TEST_CASE("ветвление и слияние: оператор после развилки собирается один раз") {
    Graph g;
    const NodeId cond = g.add(Rune::ManaAt), pos = g.add(Rune::Caster);
    const NodeId branch = g.add(Rune::JmpIf);
    const NodeId a_vec = g.add(Rune::Target), a_r = g.add(Rune::Push, (1_fx).raw), a = g.add(Rune::Carve);
    const NodeId b_vec = g.add(Rune::Target), b_r = g.add(Rune::Push, (2_fx).raw), b = g.add(Rune::Raise);
    const NodeId end = g.add(Rune::Halt);
    g.set_input(cond, 0, pos), g.set_input(branch, 0, cond);
    g.set_input(a, 0, a_vec), g.set_input(a, 1, a_r), g.set_input(b, 0, b_vec), g.set_input(b, 1, b_r);
    g.set_next(branch, a), g.set_branch(branch, b);
    g.set_next(a, end), g.set_next(b, end);
    g.entry = branch;
    const auto p = compile(g);
    REQUIRE(p.has_value());
    int halts = 0, carves = 0, raises = 0;
    for (const Instruction& i : p->code) halts += i.rune == Rune::Halt, carves += i.rune == Rune::Carve, raises += i.rune == Rune::Raise;
    CHECK(carves == 1);
    CHECK(raises == 1);
    CHECK(halts == 1); // слияние: общий HALT собран один раз, вторая ветка прыгает на него

    // Поведение: условие истинно → raise, ложно → carve.
    struct Host final : SpellHost {
        Mana density_value{};
        Math::WorldPos position(ECS::Entity) override { return {}; }
        Math::WorldPos target(ECS::Entity, Math::FVec3, Fixed) override { return {}; }
        Mana density(Math::WorldPos) override { return density_value; }
        Mana draw(Math::WorldPos, Fixed, Mana a) override { return a; }
        bool take_personal(ECS::Entity, Mana) override { return true; }
        void give_personal(ECS::Entity, Mana) override {}
    };
    for (const bool dense : {false, true}) {
        ECS::World world;
        SpellSystem system;
        Host host;
        host.density_value = dense ? Mana::from_int(5) : Mana{};
        const ECS::Entity caster = world.create();
        (void)system.cast(world, caster, std::make_shared<const Program>(*p), {}, ManaSource::Personal);
        EffectBuffer effects;
        system.tick(world, host, effects);
        REQUIRE(effects.size() == 1);
        CHECK((effects[0].kind == Effect::Kind::Raise) == dense);
    }
}

TEST_CASE("ошибки графа: типы, входы, циклы данных, операции стека") {
    {
        Graph g; // CARVE без входов
        g.entry = g.add(Rune::Carve);
        CHECK(compile(g).error().code == Code::WrongInputCount);
        CHECK(compile(g).error().node == g.entry);
    }
    {
        Graph g; // радиус подключён вектором
        const NodeId v = g.add(Rune::Target), c = g.add(Rune::Carve);
        g.set_input(c, 0, v), g.set_input(c, 1, v);
        g.entry = c;
        CHECK(compile(g).error().code == Code::WrongInputType);
    }
    {
        Graph g; // цикл по данным
        const NodeId a = g.add(Rune::Add), b = g.add(Rune::Add), j = g.add(Rune::JmpIf);
        g.set_input(a, 0, b), g.set_input(a, 1, b), g.set_input(b, 0, a), g.set_input(b, 1, a), g.set_input(j, 0, a);
        g.set_branch(j, j);
        g.entry = j;
        CHECK(compile(g).error().code == Code::DataCycle);
    }
    {
        Graph g;
        const NodeId dup = g.add(Rune::Dup), j = g.add(Rune::JmpIf);
        g.set_input(j, 0, dup);
        g.set_branch(j, j);
        g.entry = j;
        CHECK(compile(g).error().code == Code::StackRune);
    }
    {
        Graph g;
        CHECK(compile(g).error().code == Code::NoEntry);
        const NodeId j = g.add(Rune::JmpIf), one = g.add(Rune::Push);
        g.set_input(j, 0, one);
        g.entry = j; // без ветки
        CHECK(compile(g).error().code == Code::MissingBranch);
    }
    {
        Graph g; // значение в цепочке управления
        g.entry = g.add(Rune::Push);
        CHECK_FALSE(compile(g).has_value());
    }
}

TEST_CASE("слишком длинный граф отвергается") {
    Graph g;
    NodeId prev = no_node;
    for (int i = 0; i < 260; ++i) {
        const NodeId v = g.add(Rune::Target), r = g.add(Rune::Push, (1_fx).raw), c = g.add(Rune::Carve);
        g.set_input(c, 0, v), g.set_input(c, 1, r);
        if (prev == no_node) g.entry = c;
        else g.set_next(prev, c);
        prev = c;
    }
    CHECK(compile(g).error().code == Code::ProgramTooLong);
}

TEST_CASE("недостижимые узлы игнорируются, а сохранение графа — туда и обратно") {
    Graph g = carve_graph();
    (void)g.add(Rune::Mul, 0, 500, 500); // «мусор» в редакторе
    const auto p = compile(g);
    REQUIRE(p.has_value());
    CHECK(p->code.size() == 4);

    const std::string text = serialize(g);
    const auto back = parse_graph(text);
    REQUIRE(back.has_value());
    CHECK(*back == g);
    CHECK(serialize(*back) == text);
    CHECK(text.find("node 2 PUSH value 2") != std::string::npos);
}

TEST_CASE("файл графа: номера с дырами, дробные значения, ошибки с номером строки") {
    const auto g = parse_graph("# комментарий\nentry 3\nnode 1 target at 1 2\nnode 2 push value -0.25 at 3 4\nnode 4 halt\nnode 3 carve in 1 2 next 4\n");
    REQUIRE(g.has_value());
    CHECK(g->find(2)->value == (-0.25_fx).raw);
    CHECK(g->find(3)->inputs == std::vector<NodeId>{1, 2});
    CHECK(g->find(3)->next == 4);
    CHECK(g->entry == 3);
    CHECK(g->find(1)->x == 1.0f);

    CHECK(parse_graph("node 1 FLY\n").error().code == Code::UnknownRune);
    CHECK(parse_graph("node 1 FLY\n").error().line == 1);
    CHECK(parse_graph("entry 1\nnode 1 PUSH value abc\n").error().code == Code::BadNumber);
    CHECK(parse_graph("entry 1\nnode 1 PUSH value abc\n").error().line == 2);
    CHECK(parse_graph("node 1 HALT\nnode 1 HALT\n").error().code == Code::DuplicateNode);
    CHECK(parse_graph("bogus\n").error().code == Code::UnknownKeyword);
    CHECK(parse_fixed("1.5").value() == (1.5_fx).raw);
    // Запись до 5 знаков: значения с «круглым» десятичным видом переживают круг без потерь.
    for (const std::int32_t raw : {0, 65536, -65536, (2.5_fx).raw, (-0.25_fx).raw, (1000.125_fx).raw}) CHECK(parse_fixed(format_fixed(raw)).value() == raw);
    CHECK(format_fixed((0.5_fx).raw) == "0.5");
    Graph exact;
    exact.entry = exact.add(Rune::Push, 12345); // raw без короткого десятичного вида
    CHECK(parse_graph(serialize(exact))->find(1)->value == 12345);
}

TEST_CASE("обратная сборка: программа → граф → тот же байт-код") {
    for (const char* text : {"TARGET\nPUSH 2\nCARVE\nHALT\n", "CASTER\nPUSH 3\nPUSH 10\nDRAW\nDROP\nHALT\n", "loop:\nPUSH 1\nJMP_IF loop\n",
                             "CASTER\nPUSH 1\nPUSH 2\nADD\nPUSH 0.5\nMUL\nRAISE\n", "CASTER\nMANA_AT\nJMP_IF skip\nTARGET\nPUSH 1\nCARVE\nskip:\nHALT\n"}) {
        const auto program = parse_program(text);
        REQUIRE(program.has_value());
        const auto graph = decompile(*program);
        REQUIRE_MESSAGE(graph.has_value(), text);
        const auto again = compile(*graph);
        REQUIRE_MESSAGE(again.has_value(), text);
        CHECK_MESSAGE(same_code(again->code, program->code), text);
    }
}

TEST_CASE("обратная сборка: DUP выражается общим узлом, счётчик на стеке — нет") {
    const auto dup = parse_program("PUSH 3\nDUP\nADD\nDROP\nHALT\n");
    // DUP + ADD: общий узел PUSH 3 на оба входа; результат ADD снимается DROP
    const auto graph = decompile(*dup);
    REQUIRE(graph.has_value());
    int pushes = 0;
    for (const auto& [id, n] : graph->nodes()) pushes += n.rune == Rune::Push;
    CHECK(pushes == 1);

    const auto counter = parse_program("PUSH 3\nagain:\nPUSH -1\nADD\nDUP\nJMP_IF again\nHALT\n");
    const auto bad = decompile(*counter);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == Code::StackNotEmpty);
}

TEST_CASE("библиотека: .rungraph и .rune дают один байт-код, ошибка графа не ломает прежнюю программу") {
    const auto dir = std::filesystem::temp_directory_path() / "flux_runes_graph_test";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "from_text.rune") << "TARGET\nPUSH 2\nCARVE\nHALT\n";
    std::ofstream(dir / "from_graph.rungraph") << serialize(carve_graph());
    ProgramLibrary lib;
    const auto report = lib.load_directory(dir);
    CHECK(report.loaded == 2);
    CHECK(report.errors.empty());
    CHECK(same_code(lib.find("from_text")->code, lib.find("from_graph")->code));

    std::ofstream(dir / "from_graph.rungraph") << "entry 1\nnode 1 CARVE\n"; // CARVE без входов
    const auto bad = lib.load_directory(dir);
    REQUIRE(bad.errors.size() == 1);
    CHECK(bad.errors[0].find("from_graph.rungraph") != std::string::npos);
    CHECK(bad.errors[0].find("узел 1") != std::string::npos); // место ошибки в отчёте: узел графа
    CHECK(lib.find("from_graph")->code.size() == 4); // прежняя
    std::filesystem::remove_all(dir);
}
