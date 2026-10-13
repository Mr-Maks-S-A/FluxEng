#include <SpellSim/SpellSim.hpp>

#include <doctest/doctest.h>

using namespace SpellSim;
using Math::Fixed;
using Math::FVec3;

namespace {

const FVec3 look_down{Fixed{}, Fixed::from_int(-1), Fixed{}};

Config ranked() {
    Config config;
    config.use_ranks = true;
    config.player_mana = Math::Mana::from_int(30000); // шар радиуса 3 стоит 810, Fixed насыщается на 32767: хватит на 30 шаров
    return config;
}

std::uint64_t provide(Simulation& sim, const char* text) {
    auto program = Runes::parse_program(text);
    REQUIRE(program.has_value());
    const auto hash = sim.provide_program(Runes::encode_program(*program));
    REQUIRE(hash.has_value());
    return *hash;
}

} // namespace

TEST_CASE("ранги: по умолчанию выключены — опыт не копится, хеш mastery не появляется") {
    Simulation sim;
    const std::uint64_t h = provide(sim, "TARGET\nPUSH 2\nCARVE\nHALT\n");
    sim.set_grimoire_slot(0, Runes::program_name_for_hash(h));
    const Replay::Command cast = cast_command(0, Runes::ManaSource::Personal, look_down);
    sim.tick(std::span(&cast, 1));
    for (int i = 0; i < 4; ++i) sim.tick({});
    CHECK(sim.casts() == 1);
    CHECK(sim.mastery().xp == 0);
    CHECK_FALSE(sim.hashes().find("mastery").has_value());
}

TEST_CASE("ранги: новичку закрыта руна переменной; каст отклоняется и считается") {
    Simulation sim(ranked());
    const std::uint64_t h = provide(sim, "PUSH 1\nSTORE 0\nTARGET\nPUSH 1\nCARVE\nHALT\n");
    sim.set_grimoire_slot(0, Runes::program_name_for_hash(h));
    const Replay::Command cast = cast_command(0, Runes::ManaSource::Personal, look_down);
    sim.tick(std::span(&cast, 1));
    for (int i = 0; i < 4; ++i) sim.tick({});
    CHECK(sim.casts() == 0);
    CHECK(sim.locked_casts() == 1);
    CHECK(sim.hashes().find("mastery").has_value());
}

TEST_CASE("ранги: эффекты приносят опыт, ранг растёт, максимум маны растёт, закрытая руна открывается") {
    Simulation sim(ranked());
    const std::uint64_t carve = provide(sim, "TARGET\nPUSH 3\nCARVE\nHALT\n");   // шар радиуса 3 м: 10 опыта
    const std::uint64_t vars = provide(sim, "PUSH 1\nSTORE 0\nTARGET\nPUSH 1\nCARVE\nHALT\n");
    sim.set_grimoire_slot(0, Runes::program_name_for_hash(carve));
    sim.set_grimoire_slot(1, Runes::program_name_for_hash(vars));
    const Math::Mana max_before = sim.world().get<Character::ManaPool>(sim.player())->max;

    const Replay::Command locked = cast_command(1, Runes::ManaSource::Personal, look_down);
    sim.tick(std::span(&locked, 1));
    CHECK(sim.locked_casts() == 1);

    // 30 шаров по 10 опыта = 300 → ранг 2 «Знаток».
    const Replay::Command cast = cast_command(0, Runes::ManaSource::Personal, look_down);
    for (int i = 0; i < 30; ++i) {
        sim.tick(std::span(&cast, 1));
        for (int j = 0; j < 30; ++j) sim.tick({});
    }
    CHECK(sim.mastery().rank >= 2);
    CHECK(sim.rank_ups() >= 2);
    CHECK(sim.world().get<Character::ManaPool>(sim.player())->max > max_before);

    const std::uint32_t casts_before = sim.casts();
    sim.tick(std::span(&locked, 1)); // теперь переменные открыты
    for (int i = 0; i < 4; ++i) sim.tick({});
    CHECK(sim.casts() == casts_before + 1);
}

TEST_CASE("ранги: путь чародея одинаков при повторе — те же команды дают те же хеши") {
    const auto run = [] {
        Simulation sim(ranked());
        const std::uint64_t h = provide(sim, "TARGET\nPUSH 2\nCARVE\nHALT\n");
        sim.set_grimoire_slot(0, Runes::program_name_for_hash(h));
        const Replay::Command cast = cast_command(0, Runes::ManaSource::Personal, look_down);
        for (int i = 0; i < 10; ++i) {
            sim.tick(std::span(&cast, 1));
            for (int j = 0; j < 20; ++j) sim.tick({});
        }
        return sim.hashes();
    };
    const Replay::StateHashes a = run(), b = run();
    CHECK(a == b);
    CHECK(a.at("mastery") != 0);
}
