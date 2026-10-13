#include <SpellSim/SpellSim.hpp>

#include <doctest/doctest.h>

using namespace SpellSim;
using Math::Fixed;
using Math::FVec3;

namespace {

Config two_players() {
    Config config;
    config.player_count = 2;
    return config;
}

const FVec3 look_down{Fixed{}, Fixed::from_int(-1), Fixed{}};

} // namespace

TEST_CASE("игроки: один по умолчанию, у каждого свой маг, спавн с шагом") {
    Simulation single;
    CHECK(single.players().size() == 1);
    CHECK(single.player() == single.players()[0]);

    Config config = two_players();
    config.player_count = 3;
    Simulation sim(config);
    REQUIRE(sim.players().size() == 3);
    const auto x = [&](std::size_t i) { return sim.world().get<Character::Position>(sim.player(i))->value.x; };
    CHECK(x(1) - x(0) == static_cast<std::int64_t>(config.player_spacing_m) * Fixed::one_raw);
    CHECK(x(2) - x(1) == x(1) - x(0));
}

TEST_CASE("игроки: команда управляет только своим магом") {
    Simulation sim(two_players());
    const auto p0 = sim.world().get<Character::Position>(sim.player(0))->value;
    const auto p1 = sim.world().get<Character::Position>(sim.player(1))->value;
    const Replay::Command go = move_command(Fixed::from_int(1), Fixed{}).with_player(1);
    for (int i = 0; i < 30; ++i) sim.tick(std::span(&go, 1));
    CHECK(sim.world().get<Character::Position>(sim.player(0))->value.x == p0.x); // игрок 0 стоит
    CHECK(sim.world().get<Character::Position>(sim.player(1))->value.x > p1.x);  // игрок 1 идёт
}

TEST_CASE("игроки: каст тратит ману только кастующего, гримуар у каждого свой") {
    Simulation sim(two_players());
    auto program = Runes::parse_program("TARGET\nPUSH 3\nCARVE\nHALT\n");
    REQUIRE(program.has_value());
    const auto hash = sim.provide_program(Runes::encode_program(*program));
    REQUIRE(hash.has_value());
    sim.set_grimoire_slot(0, Runes::program_name_for_hash(*hash), 1); // заклинание только у игрока 1
    const Math::Mana before0 = sim.world().get<Character::ManaPool>(sim.player(0))->current;
    const Math::Mana before1 = sim.world().get<Character::ManaPool>(sim.player(1))->current;
    const Replay::Command cast = cast_command(0, Runes::ManaSource::Personal, look_down).with_player(1);
    sim.tick(std::span(&cast, 1));
    for (int i = 0; i < 4; ++i) sim.tick({});
    CHECK(sim.casts() == 1);
    CHECK(sim.world().get<Character::ManaPool>(sim.player(0))->current >= before0); // не списано (только регенерация)
    CHECK(sim.world().get<Character::ManaPool>(sim.player(1))->current < before1);
}

TEST_CASE("игроки: команда несуществующему игроку отбрасывается и считается, мир не меняется") {
    Simulation a(two_players()), b(two_players());
    const Replay::Command stray = jump_command().with_player(9);
    a.tick(std::span(&stray, 1));
    b.tick({});
    CHECK(a.unknown_player_commands() == 1);
    CHECK(a.hashes().differing(b.hashes()) == std::vector<std::string>{});
}

TEST_CASE("игроки: SetProgram правит гримуар указанного игрока (писарь — пилоту)") {
    Simulation sim(two_players());
    auto program = Runes::parse_program("TARGET\nPUSH 3\nCARVE\nHALT\n");
    REQUIRE(program.has_value());
    const auto hash = sim.provide_program(Runes::encode_program(*program));
    REQUIRE(hash.has_value());

    const Replay::Command to_pilot = set_program_command(1, *hash, 0).with_player(1);
    const Replay::Command to_self = set_program_command(2, *hash).with_player(1);
    const Replay::Command to_nobody = set_program_command(0, *hash, 5).with_player(1);
    const Replay::Command batch[] = {to_pilot, to_self, to_nobody};
    sim.tick(batch);
    const std::string name = Runes::program_name_for_hash(*hash);
    CHECK(sim.world().get<Character::Grimoire>(sim.player(0))->slots[1] == name);
    CHECK(sim.world().get<Character::Grimoire>(sim.player(1))->slots[2] == name);
    CHECK(sim.programs_set() == 2);
    CHECK(sim.rejected_programs() == 1);
    CHECK(SetProgramCommand::decode(to_pilot).target == 0);
    CHECK(SetProgramCommand::decode(to_self).target == -1);
}

TEST_CASE("игроки: два мага не проходят друг сквозь друга и повтор побитно одинаков") {
    const auto run = [] {
        Simulation sim(two_players());
        // Игрок 1 идёт влево прямо на игрока 0.
        const Replay::Command go = move_command(Fixed::from_int(-1), Fixed{}).with_player(1);
        for (int i = 0; i < 120; ++i) sim.tick(std::span(&go, 1));
        return sim.hashes();
    };
    Simulation sim(two_players());
    const Replay::Command go = move_command(Fixed::from_int(-1), Fixed{}).with_player(1);
    for (int i = 0; i < 120; ++i) sim.tick(std::span(&go, 1));
    const auto& p0 = sim.world().get<Character::Position>(sim.player(0))->value;
    const auto& p1 = sim.world().get<Character::Position>(sim.player(1))->value;
    const std::int64_t dx = p1.x - p0.x, dz = p1.z - p0.z;
    const std::int64_t need = 2 * (Fixed::from_ratio(2, 5).raw) - 64; // суммы радиусов минус допуск округления
    CHECK(dx * dx + dz * dz >= need * need);
    CHECK(run() == run());
}
