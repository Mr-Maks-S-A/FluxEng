#include <Runes/Runes.hpp>

#include <doctest/doctest.h>

#include <fstream>
#include <cmath>

using namespace Runes;
using Math::Fixed;
using Math::Mana;
using Math::FVec3;
using Math::WorldPos;
using Math::literals::operator""_fx;
using Math::literals::operator""_mana;

namespace {

/// Мир-заглушка: запас мага, поле как одно число, журнал эффектов.
struct MockHost final : SpellHost {
    Mana personal = Mana::from_int(1000);
    Mana field = Mana::from_int(100000);
    Mana field_spent{};
    WorldPos caster_pos = WorldPos::from_meters(10, 5, 10);
    WorldPos target_pos = WorldPos::from_meters(20, 4, 20);

    WorldPos position(ECS::Entity) override { return caster_pos; }
    WorldPos target(ECS::Entity, FVec3, Fixed) override { return target_pos; }
    Mana density(WorldPos) override { return 7_mana; }
    Mana draw(WorldPos, Fixed, Mana amount) override {
        const Mana got = Math::min(amount, field);
        field -= got;
        field_spent += got;
        return got;
    }
    bool take_personal(ECS::Entity, Mana amount) override {
        if (amount > personal) return false;
        personal -= amount;
        return true;
    }
    void give_personal(ECS::Entity, Mana amount) override { personal += amount; }
};

struct Rig {
    ECS::World world;
    SpellSystem system;
    MockHost host;
    ECS::Entity caster = world.create();
    ProgramLibrary library;
    EffectBuffer effects; ///< Эффекты заклинаний — данные, мир их здесь не применяется.

    [[nodiscard]] std::vector<std::pair<WorldPos, Fixed>> of(Effect::Kind kind) const {
        std::vector<std::pair<WorldPos, Fixed>> out;
        for (const Effect& e : effects) if (e.kind == kind) out.emplace_back(e.position, e.radius);
        return out;
    }
    [[nodiscard]] auto carved() const { return of(Effect::Kind::Carve); }
    [[nodiscard]] auto raised() const { return of(Effect::Kind::Raise); }

    ECS::Entity cast(std::string_view text, ManaSource source = ManaSource::Personal) {
        REQUIRE(library.add_text("spell", text).has_value());
        return system.cast(world, caster, library.find("spell"), {Fixed{}, 1_fx, Fixed{}}, source);
    }
    void run_ticks(int n) {
        for (int i = 0; i < n; ++i) system.tick(world, host, effects);
    }
};

} // namespace

TEST_CASE("разбор: руны, числа, метки, комментарии") {
    const auto program = parse_program("# вырезать\n  push 2.5   ; радиус\nstart:\nTARGET\nJMP_IF start\nhalt // конец\n", "t");
    REQUIRE(program.has_value());
    REQUIRE(program->code.size() == 4);
    CHECK(program->code[0].rune == Rune::Push);
    CHECK(program->code[0].operand == (2.5_fx).raw);
    CHECK(program->code[2].rune == Rune::JmpIf);
    CHECK(program->code[2].operand == 1); // метка start → руна TARGET
    CHECK(parse_program("PUSH -0.25")->code[0].operand == (-0.25_fx).raw);
}

TEST_CASE("проверка при загрузке: известные руны и цели переходов") {
    CHECK(parse_program("FLY").error().code == Code::UnknownRune);
    CHECK(parse_program("FLY").error().detail == "FLY");
    CHECK(parse_program("PUSH").error().code == Code::MissingOperand);
    CHECK(parse_program("PUSH").error().line == 1);
    CHECK(parse_program("DUP 3").error().code == Code::UnexpectedOperand);
    CHECK(parse_program("PUSH abc").error().code == Code::BadNumber);
    CHECK(parse_program("JMP_IF nowhere").error().code == Code::UnknownLabel);
    CHECK(parse_program("PUSH 1\nJMP_IF 5").error().code == Code::JumpOutOfRange);
    CHECK(parse_program("PUSH 99999").error().code == Code::NumberOutOfRange);
    CHECK_FALSE(parse_program("# только комментарий").has_value());
    std::string long_program;
    for (int i = 0; i < 257; ++i) long_program += "PUSH 1\n";
    CHECK(parse_program(long_program).error().code == Code::ProgramTooLong);
    std::string max_program;
    for (int i = 0; i < 256; ++i) max_program += "PUSH 1\n";
    CHECK(parse_program(max_program).has_value());
}

TEST_CASE("вырезать сферу: TARGET и радиус превращаются в эффект") {
    Rig rig;
    rig.cast("TARGET\nPUSH 2\nCARVE\nHALT\n");
    rig.run_ticks(1);
    REQUIRE(rig.carved().size() == 1);
    CHECK(rig.carved()[0].first == rig.host.target_pos);
    CHECK(rig.carved()[0].second == 2_fx);
    CHECK(rig.system.active(rig.world) == 0); // заклинание закончилось
    // Цена: 4 руны × 0,05 + 30 · 2³ (эффект) = 240,2
    CHECK(std::abs(rig.system.last_trace().spent.to_double() - (4 * 0.05 + 30 * 8)) < 0.01);
    CHECK(rig.host.personal.to_double() == doctest::Approx(1000 - (4 * 0.05 + 30 * 8)).epsilon(0.0001));
}

TEST_CASE("эффекты — данные: порядок, чей маг, трасса считает эффекты, мир не тронут") {
    Rig rig;
    rig.cast("TARGET\nPUSH 2\nCARVE\nCASTER\nPUSH 1\nRAISE\nHALT\n");
    rig.run_ticks(1);
    REQUIRE(rig.effects.size() == 2);
    CHECK(rig.effects[0].kind == Effect::Kind::Carve);
    CHECK(rig.effects[1].kind == Effect::Kind::Raise);
    CHECK(rig.effects[0].caster == rig.caster);
    CHECK(rig.effects[0].position == rig.host.target_pos);
    CHECK(rig.effects[1].position == rig.host.caster_pos);
    CHECK(rig.system.last_trace().effects == 2);
    // Радиус в эффекте уже зажат в допустимый диапазон.
    Rig clamp;
    clamp.cast("TARGET\nPUSH 100\nCARVE\nHALT\n");
    clamp.host.personal = Mana::from_int(1'000'000);
    clamp.system = SpellSystem(Tuning{.max_budget = Mana::from_int(1'000'000)});
    clamp.cast("TARGET\nPUSH 100\nCARVE\nHALT\n");
    clamp.run_ticks(1);
    REQUIRE_FALSE(clamp.effects.empty());
    CHECK(clamp.effects.back().radius == clamp.system.tuning().max_radius);
}

TEST_CASE("поднять землю и арифметика") {
    Rig rig;
    rig.cast("CASTER\nPUSH 1\nPUSH 2\nADD\nPUSH 0.5\nMUL\nRAISE\n"); // радиус (1+2)·0,5 = 1,5; без HALT: конец программы
    rig.run_ticks(1);
    REQUIRE(rig.raised().size() == 1);
    CHECK(rig.raised()[0].first == rig.host.caster_pos);
    CHECK(rig.raised()[0].second == 1.5_fx);
}

TEST_CASE("контекст и чувство: MANA_AT, DRAW пополняет мага") {
    Rig rig;
    rig.host.personal = 100_mana;
    rig.cast("CASTER\nPUSH 3\nPUSH 10\nDRAW\nDROP\nCASTER\nMANA_AT\nHALT\n");
    rig.run_ticks(1);
    // DRAW вернул 10 в запас, остальные руны по 0,05; MANA_AT дал 7.
    CHECK(rig.host.personal.to_double() == doctest::Approx(100 + 10 - 8 * 0.05).epsilon(0.001));
    CHECK(rig.host.field_spent == 10_mana);
}

TEST_CASE("условный переход: цикл по счётчику") {
    Rig rig;
    // счётчик 3: пока не ноль — вычесть 1 (ADD −1) и повторить; считаем проходы по расходу
    rig.cast("PUSH 3\nloop:\nPUSH -1\nADD\nDUP\nJMP_IF loop\nHALT\n");
    rig.run_ticks(1);
    CHECK(rig.system.active(rig.world) == 0);
    CHECK(rig.system.last_trace().status == Status::Halted);
    CHECK(rig.system.last_trace().runes_executed == 1 + 3 * 4 + 1);
}

TEST_CASE("ошибки останавливают заклинание событием, движок не падает") {
    EventSystem::EventBus bus;
    Rig rig;
    rig.system.declare(bus);
    const EventSystem::ModuleId watcher = bus.declare_module("Watcher").consumes<SpellFailedEvent>();
    auto failures = bus.reader<SpellFailedEvent>(watcher);

    rig.cast("ADD\n"); // пустой стек
    rig.run_ticks(1);
    CHECK(rig.system.last_trace().failure == Failure::StackUnderflow);
    CHECK(rig.system.active(rig.world) == 0);
    bus.advance_tick();
    REQUIRE(failures.events().size() == 1);
    CHECK(failures.events()[0].reason == static_cast<std::uint32_t>(Failure::StackUnderflow));

    std::string overflow;
    for (int i = 0; i < 65; ++i) overflow += "PUSH 1\n";
    rig.cast(overflow);
    rig.run_ticks(1);
    CHECK(rig.system.last_trace().failure == Failure::StackOverflow);

    rig.cast("PUSH 1\nPUSH 1\nPUSH 1\nPUSH 1\nCARVE\n"); // нормально: вектор (1,1,1) радиус 1
    rig.run_ticks(1);
    CHECK(rig.system.last_trace().status == Status::Halted);
    rig.cast("PUSH 1\nCARVE\n"); // не хватает значений под CARVE
    rig.run_ticks(1);
    CHECK(rig.system.last_trace().failure == Failure::StackUnderflow);
}

TEST_CASE("бесконечный цикл останавливается сам, когда кончается мана") {
    Rig rig;
    rig.host.personal = 5_mana;
    rig.cast("loop:\nPUSH 1\nJMP_IF loop\n");
    int ticks = 0;
    while (rig.system.active(rig.world) > 0 && ticks < 1000) {
        rig.run_ticks(1);
        ++ticks;
    }
    CHECK(rig.system.active(rig.world) == 0);
    CHECK(ticks < 1000);
    CHECK(rig.system.last_trace().failure == Failure::OutOfMana);
    CHECK(rig.system.last_trace().runes_executed >= 90); // 5 маны / 0,05
    CHECK(rig.host.personal.to_double() < 0.06);
}

TEST_CASE("за тик исполняется не больше 256 рун и заклинание продолжается") {
    Rig rig;
    rig.host.personal = 100000_mana;
    rig.system = SpellSystem(Tuning{.rune_cost = Mana::from_raw(1), .max_budget = 100000_mana});
    rig.cast("loop:\nPUSH 1\nJMP_IF loop\n");
    rig.run_ticks(1);
    CHECK(rig.system.last_trace().runes_executed == 256);
    CHECK(rig.system.active(rig.world) == 1);
    rig.run_ticks(2);
    CHECK(rig.system.last_trace().runes_executed == 768);
}

TEST_CASE("предел бюджета заклинания") {
    Rig rig;
    rig.host.personal = 1'000'000_mana;
    rig.system = SpellSystem(Tuning{.max_budget = 10_mana});
    rig.cast("loop:\nPUSH 1\nJMP_IF loop\n");
    rig.run_ticks(5);
    CHECK(rig.system.last_trace().failure == Failure::OutOfBudget);
}

TEST_CASE("окружающая мана: платит поле, маг платит в N раз меньше") {
    Rig rig;
    rig.cast("TARGET\nPUSH 2\nCARVE\n", ManaSource::Ambient);
    rig.run_ticks(1);
    const double cost = 3 * 0.05 + 30 * 8;
    CHECK(rig.host.field_spent.to_double() == doctest::Approx(cost).epsilon(0.001));
    CHECK((1000 - rig.host.personal.to_double()) == doctest::Approx(cost / 10).epsilon(0.001));
    CHECK(rig.carved().size() == 1);

    Rig personal;
    personal.cast("TARGET\nPUSH 2\nCARVE\n", ManaSource::Personal);
    personal.run_ticks(1);
    CHECK((1000 - personal.host.personal.to_double()) == doctest::Approx(cost).epsilon(0.001)); // в 10 раз дороже
}

TEST_CASE("если поле отдало меньше нужного, заклинание останавливается на неоплаченной руне") {
    Rig rig;
    rig.host.field = 50_mana; // эффект стоит ~240
    rig.cast("TARGET\nPUSH 2\nCARVE\nHALT\n", ManaSource::Ambient);
    rig.run_ticks(1);
    CHECK(rig.system.last_trace().failure == Failure::OutOfMana);
    CHECK(rig.system.last_trace().failed_pc == 2); // CARVE
    CHECK(rig.carved().empty());
}

TEST_CASE("заклинания исполняются в порядке id, независимо от порядка в пуле") {
    struct OrderHost final : SpellHost {
        std::vector<std::int32_t> order;
        WorldPos position(ECS::Entity) override { return {}; }
        WorldPos target(ECS::Entity, FVec3, Fixed) override { return {}; }
        Mana density(WorldPos) override { return {}; }
        Mana draw(WorldPos, Fixed, Mana a) override { return a; }
        bool take_personal(ECS::Entity, Mana) override { return true; }
        void give_personal(ECS::Entity, Mana) override {}
    };
    ECS::World world;
    SpellSystem system;
    OrderHost host;
    ProgramLibrary lib;
    const ECS::Entity caster = world.create();
    std::vector<ECS::Entity> spells;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(lib.add_text("s", "PUSH " + std::to_string(i) + "\nPUSH 0\nPUSH 0\nPUSH 1\nCARVE\n").has_value());
        spells.push_back(system.cast(world, caster, lib.find("s"), {}, ManaSource::Personal));
    }
    world.destroy(spells[1]); // перетасовывает плотный массив пула
    spells.push_back(system.cast(world, caster, lib.find("s"), {}, ManaSource::Personal)); // займёт слот спелла 1
    // Новая сущность заняла освободившийся слот spells[1], а в плотном массиве пула лежит последней.
    REQUIRE(spells.back().index == spells[1].index);
    EffectBuffer effects;
    system.tick(world, host, effects);
    for (const Effect& e : effects) host.order.push_back(static_cast<std::int32_t>(e.position.x / Fixed::one_raw));
    // Программа "s" перезаписывается в цикле: новая сущность несёт x = 4. Порядок — по индексу сущности: 1, 2(новая), 3, 4, 5.
    CHECK(host.order == std::vector<std::int32_t>{0, 4, 2, 3, 4});
}

TEST_CASE("библиотека: перезагрузка каталога и ошибки не ломают прежнюю программу") {
    const auto dir = std::filesystem::temp_directory_path() / "flux_runes_test";
    std::filesystem::create_directories(dir);
    const auto write = [&](const char* name, const char* text) { std::ofstream(dir / name) << text; };
    write("good.rune", "PUSH 1\nHALT\n");
    write("bad.rune", "NOPE\n");
    ProgramLibrary lib;
    const auto first = lib.load_directory(dir);
    CHECK(first.loaded == 1);
    REQUIRE(first.errors.size() == 1);
    CHECK(first.errors[0].find("bad.rune") != std::string::npos);
    CHECK(first.errors[0].find("строка 1") != std::string::npos);
    const auto before = lib.find("good");
    write("good.rune", "PUSH 2\nPUSH 3\nHALT\n");
    CHECK(lib.load_directory(dir).loaded == 1);
    CHECK(before->code.size() == 2);            // идущее заклинание держит старую программу
    CHECK(lib.find("good")->code.size() == 3);  // новое — новую
    write("good.rune", "BROKEN\n");
    (void)lib.load_directory(dir);
    CHECK(lib.find("good")->code.size() == 3);  // ошибка в файле — остаётся прежняя
    std::filesystem::remove_all(dir);
}

TEST_CASE("хеш заклинаний зависит от состояния машины") {
    Rig a, b;
    a.host.personal = b.host.personal = 100000_mana;
    a.cast("loop:\nPUSH 1\nJMP_IF loop\n");
    b.cast("loop:\nPUSH 1\nJMP_IF loop\n");
    Math::Hasher ha, hb;
    a.run_ticks(2), b.run_ticks(2);
    hash_spells(a.world, ha), hash_spells(b.world, hb);
    CHECK(ha.value() == hb.value());
    b.run_ticks(1);
    Math::Hasher hc;
    hash_spells(b.world, hc);
    CHECK(hc.value() != ha.value());
}

TEST_CASE("Diagnostic: у каждого кода есть имя и текст, место попадает в format()") {
    for (int c = 0; c <= static_cast<int>(Code::UnusedValue); ++c) {
        const Diagnostic d{static_cast<Code>(c), 0, no_node, "x"};
        CHECK(code_name(d.code) != "?");
        CHECK_FALSE(d.message().empty());
    }
    CHECK(code_name(Code::UnknownRune) == "unknown_rune");
    CHECK(Diagnostic{Code::UnknownLabel, 3, no_node, "loop"}.format() == "строка 3: неизвестная метка: loop");
    CHECK(Diagnostic{Code::DataCycle, 0, 7}.format().starts_with("узел 7: "));
    CHECK(Diagnostic{Code::NoEntry} == Diagnostic{Code::NoEntry});
    // Один и тот же код — одна и та же логика, независимо от того, кто нашёл ошибку: текст, граф или файл.
    CHECK(parse_program("FLY").error().code == parse_graph("node 1 FLY\n").error().code);
}

TEST_CASE("estimate_cost: радиус эффекта из констант, неизвестный радиус помечен, цикл найден") {
    const Tuning tuning;
    const auto carve = parse_program("TARGET\nPUSH 2\nCARVE\nHALT\n").value();
    const CostEstimate c = estimate_cost(carve, tuning);
    CHECK(c.exact);
    CHECK_FALSE(c.loops);
    CHECK(c.runes == 4);
    CHECK(c.effects == 1);
    CHECK(c.per_pass.to_double() == doctest::Approx(4 * 0.05 + 30 * 8).epsilon(0.001)); // 4 руны + эффект 30·2³

    const auto computed = parse_program("CASTER\nPUSH 1\nPUSH 2\nADD\nPUSH 0.5\nMUL\nRAISE\n").value(); // радиус (1+2)·0.5 = 1.5 — константа
    const CostEstimate k = estimate_cost(computed, tuning);
    CHECK(k.exact);
    CHECK(k.per_pass.to_double() == doctest::Approx(7 * 0.05 + 30 * 3.375).epsilon(0.001));

    const auto dynamic = parse_program("TARGET\nCASTER\nMANA_AT\nCARVE\nHALT\n").value(); // радиус — плотность маны: неизвестен до запуска
    CHECK_FALSE(estimate_cost(dynamic, tuning).exact);

    const auto looping = parse_program("loop:\nPUSH 1\nJMP_IF loop\n").value();
    const CostEstimate l = estimate_cost(looping, tuning);
    CHECK(l.loops);
    CHECK(l.effects == 0);

    // Оценка совпадает с фактической ценой: исполняем и сравниваем.
    ECS::World world;
    SpellSystem system;
    MockHost host;
    EffectBuffer effects;
    const ECS::Entity caster = world.create();
    (void)system.cast(world, caster, std::make_shared<const Program>(carve), {}, ManaSource::Personal);
    system.tick(world, host, effects);
    CHECK(system.last_trace().spent.to_double() == doctest::Approx(c.per_pass.to_double()).epsilon(0.001));
}

TEST_CASE("add_program: готовая программа попадает в библиотеку, имя заменяется") {
    ProgramLibrary lib;
    lib.add_program("edit", parse_program("HALT\n").value());
    CHECK(lib.find("edit")->code.size() == 1);
    CHECK(lib.find("edit")->name == "edit");
    lib.add_program("edit", parse_program("PUSH 1\nHALT\n").value());
    CHECK(lib.find("edit")->code.size() == 2);
}
