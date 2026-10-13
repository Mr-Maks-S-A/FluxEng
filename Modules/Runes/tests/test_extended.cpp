// Переменные, вычитание, сравнение, безусловный переход и подпрограммы (руны, добавленные после первой версии языка).
#include <Runes/Runes.hpp>

#include <doctest/doctest.h>

using namespace Runes;
using Math::Fixed;
using Math::Mana;
using Math::FVec3;
using Math::WorldPos;

namespace {

struct Host final : SpellHost {
    Mana personal = Mana::from_int(10000);
    WorldPos position(ECS::Entity) override { return WorldPos::from_meters(10, 5, 10); }
    WorldPos target(ECS::Entity, FVec3, Fixed) override { return WorldPos::from_meters(20, 4, 20); }
    Mana density(WorldPos) override { return Mana::from_int(7); }
    Mana draw(WorldPos, Fixed, Mana) override { return {}; }
    bool take_personal(ECS::Entity, Mana amount) override {
        if (amount > personal) return false;
        personal -= amount;
        return true;
    }
    void give_personal(ECS::Entity, Mana amount) override { personal += amount; }
};

struct Machine {
    ECS::World world;
    SpellSystem system;
    Host host;
    EffectBuffer effects;
    ECS::Entity caster = world.create();
    std::vector<Failure> failures;

    /// Запускает программу и крутит тики до остановки; возвращает число эффектов (CARVE).
    std::size_t run(std::string_view text, int max_ticks = 50) {
        const auto program = parse_program(text, "t");
        REQUIRE(program.has_value());
        const ECS::Entity spell = system.cast(world, caster, std::make_shared<const Program>(*program), {Fixed{}, Fixed::from_int(1), Fixed{}}, ManaSource::Personal);
        for (int i = 0; i < max_ticks && world.get<MachineState>(spell) != nullptr; ++i) system.tick(world, host, effects);
        return effects.size();
    }
};

} // namespace

TEST_CASE("новые руны: разбор, имена и операнды") {
    const auto p = parse_program("PUSH 1\nSTORE 3\nLOAD 3\nPUSH 2\nSUB\nPUSH 0\nLT\nJMP end\nCALL end\nend:\nRET\n", "t");
    REQUIRE(p.has_value());
    REQUIRE(p->code.size() == 10); // метка не руна
    CHECK(p->code[1].rune == Rune::Store);
    CHECK(p->code[1].operand == 3);
    CHECK(p->code[7].rune == Rune::Jmp);
    CHECK(p->code[7].operand == 9); // метка end → RET
    CHECK(p->code[8].rune == Rune::Call);
    CHECK(p->code[8].operand == 9);
    CHECK(disassemble(*p).find("STORE 3") != std::string::npos);
    CHECK(parse_program("STORE 8").error().code == Code::BadVariable);
    CHECK(parse_program("LOAD -1").error().code == Code::BadVariable);
    CHECK(parse_program("LOAD x").error().code == Code::BadVariable);
    CHECK(parse_program("STORE").error().code == Code::MissingOperand);
    CHECK(parse_program("RET 1").error().code == Code::UnexpectedOperand);
    CHECK(parse_program("JMP nowhere").error().code == Code::UnknownLabel);
    CHECK(parse_program("JMP 99").error().code == Code::JumpOutOfRange);
}

TEST_CASE("прежние руны не сдвинулись: номера рун в формате Wire те же") {
    CHECK(static_cast<int>(Rune::Push) == 0);
    CHECK(static_cast<int>(Rune::Raise) == 13);
    CHECK(static_cast<int>(Rune::Sub) == 14);
    CHECK(static_cast<int>(Rune::Count) == 21);
}

TEST_CASE("переменные и арифметика: счётчик цикла считает ровно") {
    // Три раза вырезать шар: x := 3; пока 0 < x: CARVE; x := x − 1.
    Machine m;
    const std::size_t carved = m.run(
        "PUSH 3\nSTORE 0\n"
        "loop:\n"
        "TARGET\nPUSH 1\nCARVE\n"
        "LOAD 0\nPUSH 1\nSUB\nSTORE 0\n"
        "PUSH 0\nLOAD 0\nLT\n"        // 0 < x
        "JMP_IF loop\nHALT\n");
    CHECK(carved == 3);
}

TEST_CASE("LT: меньше — единица, не меньше — ноль") {
    Machine m;
    // 2 < 5 → 1 → один CARVE; 5 < 2 → 0 → CARVE не выполняется.
    CHECK(m.run("PUSH 2\nPUSH 5\nLT\nJMP_IF yes\nHALT\nyes:\nTARGET\nPUSH 1\nCARVE\nHALT\n") == 1);
    Machine n;
    CHECK(n.run("PUSH 5\nPUSH 2\nLT\nJMP_IF yes\nHALT\nyes:\nTARGET\nPUSH 1\nCARVE\nHALT\n") == 0);
}

TEST_CASE("CALL и RET: подпрограмма исполняется и возвращает управление; вложенность и RET без вызова") {
    Machine m;
    // Два вызова одной подпрограммы, затем вызов вложенной.
    CHECK(m.run("CALL carve_one\nCALL carve_one\nCALL twice\nHALT\n"
                "carve_one:\nTARGET\nPUSH 1\nCARVE\nRET\n"
                "twice:\nCALL carve_one\nCALL carve_one\nRET\n") == 4);
    Machine n;
    CHECK(n.run("TARGET\nPUSH 1\nCARVE\nRET\nTARGET\nPUSH 1\nCARVE\n") == 1); // RET без вызова — конец заклинания
}

TEST_CASE("JMP: безусловный переход без «PUSH 1; JMP_IF»") {
    Machine m;
    CHECK(m.run("JMP skip\nTARGET\nPUSH 1\nCARVE\nskip:\nTARGET\nPUSH 1\nCARVE\nHALT\n") == 1);
}

TEST_CASE("отказы: переполнение вызовов останавливает заклинание, а не движок; бесконечный цикл гасит мана") {
    Machine m;
    const auto program = parse_program("loop:\nCALL loop\n", "t");
    REQUIRE(program.has_value());
    const ECS::Entity spell = m.system.cast(m.world, m.caster, std::make_shared<const Program>(*program), {}, ManaSource::Personal);
    m.system.tick(m.world, m.host, m.effects);
    CHECK(m.world.get<MachineState>(spell) == nullptr); // заклинание уничтожено после Failed
    CHECK(failure_text(Failure::CallOverflow) == "call overflow");

    Machine n;
    n.host.personal = Mana::from_int(3);
    (void)n.run("loop:\nJMP loop\n", 200);
    CHECK(n.host.personal < Mana::from_int(1)); // цена руны вычерпала запас, цикл остановился сам
}

TEST_CASE("Wire: новые руны туда-обратно; слот и цель проверяются на границе доверия") {
    const auto p = parse_program("PUSH 1\nSTORE 7\nLOAD 7\nCALL f\nHALT\nf:\nRET\n", "t");
    REQUIRE(p.has_value());
    const auto bytes = encode_program(*p);
    const auto back = decode_program(bytes);
    REQUIRE(back.has_value());
    CHECK(back->code == p->code);

    Program bad_slot;
    bad_slot.code = {{Rune::Load, 8}, {Rune::Halt, 0}};
    CHECK(decode_program(encode_program(bad_slot)).error().code == Code::BadVariable);
    Program bad_call;
    bad_call.code = {{Rune::Call, 5}, {Rune::Halt, 0}};
    CHECK(decode_program(encode_program(bad_call)).error().code == Code::JumpOutOfRange);
}

TEST_CASE("оценка стоимости: SUB складывает константы, переходы назад — циклы") {
    const auto p = parse_program("PUSH 5\nPUSH 2\nSUB\nPUSH 1\nSUB\nTARGET\nPUSH 1\nCARVE\nHALT\n", "t");
    REQUIRE(p.has_value());
    CHECK_FALSE(estimate_cost(*p).loops);
    const auto q = parse_program("start:\nJMP start\n", "t");
    REQUIRE(q.has_value());
    CHECK(estimate_cost(*q).loops);
    const auto r = parse_program("CALL f\nHALT\nf:\nRET\n", "t");
    REQUIRE(r.has_value());
    CHECK_FALSE(estimate_cost(*r).loops);
}

TEST_CASE("граф: SUB, LT, STORE и LOAD собираются в байт-код и разбираются обратно; JMP/CALL/RET графом не выражаются") {
    const auto p = parse_program("PUSH 3\nPUSH 1\nSUB\nSTORE 2\nLOAD 2\nPUSH 5\nLT\nJMP_IF out\nHALT\nout:\nHALT\n", "t");
    REQUIRE(p.has_value());
    const auto graph = decompile(*p);
    REQUIRE(graph.has_value());
    const auto again = compile(*graph, "t");
    REQUIRE(again.has_value());
    CHECK(again->code == p->code);
    const auto text = parse_graph(serialize(*graph));
    REQUIRE(text.has_value());
    CHECK(compile(*text, "t")->code == p->code); // слот переживает сохранение графа

    const auto with_call = parse_program("CALL f\nHALT\nf:\nRET\n", "t");
    REQUIRE(with_call.has_value());
    CHECK(decompile(*with_call).error().code == Code::NotInGraph);
    Graph g;
    const NodeId jump = g.add(Rune::Jmp);
    g.entry = jump;
    CHECK(compile(g).error().code == Code::NotInGraph);
}

namespace {

/// Хеш заклинания, которое один тик крутится в цикле (живо и после тика).
std::uint64_t hash_after_one_tick(std::string_view text) {
    Machine m;
    const auto program = parse_program(text, "t");
    REQUIRE(program.has_value());
    (void)m.system.cast(m.world, m.caster, std::make_shared<const Program>(*program), {}, ManaSource::Personal);
    m.system.tick(m.world, m.host, m.effects);
    Math::Hasher hasher;
    hash_spells(m.world, hasher);
    return hasher.value();
}

} // namespace

TEST_CASE("хеш заклинаний: переменные видны в нём, а пока в них ноль, хеш прежний (golden-записи не меняются)") {
    const std::uint64_t without = hash_after_one_tick("PUSH 2\nDROP\nloop:\nJMP loop\n");
    const std::uint64_t zero = hash_after_one_tick("PUSH 0\nSTORE 0\nloop:\nJMP loop\n");
    const std::uint64_t two = hash_after_one_tick("PUSH 2\nSTORE 0\nloop:\nJMP loop\n");
    CHECK(zero == without);
    CHECK(two != without);
}
