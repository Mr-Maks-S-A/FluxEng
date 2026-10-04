#include <Challenge/Play.hpp>

#include <doctest/doctest.h>

using namespace Challenge;
using Math::Fixed;
using Math::WorldPos;

namespace {

/// Минимальный уровень «в коде»: ровная цель рядом и заданные пределы.
Level make_level(GoalKind kind = GoalKind::Reach) {
    Level l;
    l.id = "t";
    l.title = "Тест";
    l.brief = "—";
    l.config.grimoire = {"dig", "", ""};
    l.library = {{"dig", "TARGET\nPUSH 2\nCARVE\nHALT\n"}};
    l.goal = {kind, WorldPos::from_meters(64, 28, 70), Fixed::from_int(2)};
    l.limits = {.max_ticks = 600};
    return l;
}

const Math::FVec3 down{Fixed{}, Fixed::from_int(-1), Fixed{}};

Solution casts(std::initializer_list<std::uint32_t> ticks, Runes::ManaSource source = Runes::ManaSource::Personal) {
    Solution s;
    for (const std::uint32_t t : ticks) s.steps.push_back({t, SpellSim::cast_command(0, source, down)});
    return s;
}

} // namespace

TEST_CASE("Reach: победа при подходе, расстояние уменьшается") {
    Level l = make_level();
    l.goal.point = WorldPos::from_meters(64, 28, 62); // за спиной мага (он на z = 63,75 — внутри допуска)
    const Outcome o = play_idle(l);
    CHECK(o.status == Status::Won);
    CHECK(o.ticks_run <= 70);
}

TEST_CASE("Carve и Raise: цель достигается выстрелом под ноги и не достигается без него") {
    Level carve = make_level(GoalKind::Carve);
    carve.goal.point = WorldPos::from_meters(64, 27, 64); // под ногами мага: r=2 у поверхности её вскроет
    CHECK(play_idle(carve).status == Status::Lost);
    Level carve_ok = carve;
    carve_ok.limits.max_casts = 2;
    const Outcome o = play(carve_ok, casts({40}));
    CHECK(o.status == Status::Won);
    CHECK(o.metrics.casts == 1);

    Level raise = make_level(GoalKind::Raise);
    raise.config.grimoire = {"build", "", ""};
    raise.library = {{"build", "TARGET\nPUSH 2\nRAISE\nHALT\n"}};
    raise.goal.point = WorldPos::from_meters(64, 31, 64); // воздух над землёй
    CHECK(play_idle(raise).status == Status::Lost);
}

TEST_CASE("пределы: время, касты, мана, длина программы — нарушение сразу проигрыш с названной причиной") {
    {
        Level l = make_level();
        l.limits.max_ticks = 30;
        const Outcome o = play_idle(l);
        CHECK(o.status == Status::Lost);
        CHECK(o.loss == Loss::OutOfTime);
        CHECK(o.ticks_run == 30);
    }
    {
        Level l = make_level();
        l.limits.max_casts = 2;
        const Outcome o = play(l, casts({10, 20, 30}));
        CHECK(o.loss == Loss::TooManyCasts);
        CHECK(o.metrics.casts == 3);
    }
    {
        Level l = make_level();
        l.limits.max_mana_spent = 300; // два каста по 240 — уже 480
        const Outcome o = play(l, casts({10, 60}));
        CHECK(o.loss == Loss::TooMuchMana);
        CHECK(o.metrics.mana_spent > 300);
    }
    {
        Level l = make_level();
        l.limits.max_program_runes = 3; // «dig» из 4 рун — длиннее
        const Outcome o = play_idle(l);
        CHECK(o.loss == Loss::ProgramTooLong);
        CHECK(o.metrics.program_runes == 4);
    }
}

TEST_CASE("звёзды: победа — одна, мана в планке — вторая, программа в планке — третья") {
    Level l = make_level(GoalKind::Carve);
    l.goal.point = WorldPos::from_meters(64, 27, 64);
    const Solution one_cast = casts({40});

    l.par = {.mana_spent = 300, .program_runes = 4};
    CHECK(play(l, one_cast).stars == 3);
    l.par = {.mana_spent = 300, .program_runes = 3}; // программа длиннее планки
    CHECK(play(l, one_cast).stars == 2);
    l.par = {.mana_spent = 100, .program_runes = 3}; // и мана дороже планки
    CHECK(play(l, one_cast).stars == 1);
    l.par = {};                                      // планок нет — все звёзды за победу
    CHECK(play(l, one_cast).stars == 3);
    CHECK(play_idle(l).stars == 0);
}

TEST_CASE("судья замирает после исхода: метрики и статус больше не меняются") {
    Level l = make_level(GoalKind::Carve);
    l.goal.point = WorldPos::from_meters(64, 27, 64);
    SpellSim::Simulation sim(l.config);
    install_library(l, sim);
    Referee ref(l);
    const Replay::Command cast = SpellSim::cast_command(0, Runes::ManaSource::Personal, down);
    for (int t = 0; t < 40; ++t) { sim.tick({}); ref.observe(sim); }
    sim.tick(std::span(&cast, 1));
    ref.observe(sim);
    sim.tick({});
    ref.observe(sim);
    REQUIRE(ref.status() == Status::Won);
    const Metrics frozen = ref.metrics();
    const int stars = ref.stars();
    for (int t = 0; t < 600; ++t) { sim.tick(std::span(&cast, 1)); ref.observe(sim); } // дальше можно палить сколько угодно
    CHECK(ref.status() == Status::Won);
    CHECK(ref.metrics().casts == frozen.casts);
    CHECK(ref.metrics().ticks == frozen.ticks);
    CHECK(ref.stars() == stars);
}

TEST_CASE("строка HUD: цель, время, мана, касты, исход") {
    Level l = make_level();
    l.limits = {.max_ticks = 600, .max_casts = 3, .max_program_runes = 8, .max_mana_spent = 500};
    SpellSim::Simulation sim(l.config);
    install_library(l, sim);
    Referee ref(l);
    sim.tick({});
    ref.observe(sim);
    const std::string line = ref.status_line();
    CHECK(line.find("Тест") != std::string::npos);
    CHECK(line.find("/10 с") != std::string::npos);
    CHECK(line.find("/500") != std::string::npos);
    CHECK(line.find("/3") != std::string::npos);
    CHECK(line.find("до цели") != std::string::npos);
    CHECK(Referee::loss_text(Loss::OutOfTime) == "время вышло");
}

TEST_CASE("мана: траты считаются по падениям запаса, восстановление не вычитается") {
    Level l = make_level();
    SpellSim::Simulation sim(l.config);
    install_library(l, sim);
    Referee ref(l);
    const Replay::Command cast = SpellSim::cast_command(0, Runes::ManaSource::Personal, down);
    for (int t = 0; t < 200; ++t) {
        sim.tick(t == 10 || t == 100 ? std::span(&cast, 1) : std::span<const Replay::Command>{});
        ref.observe(sim);
    }
    CHECK(ref.metrics().mana_spent >= 479);
    CHECK(ref.metrics().mana_spent <= 481);
}
