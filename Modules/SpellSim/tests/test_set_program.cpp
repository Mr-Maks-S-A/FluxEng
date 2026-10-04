#include <SpellSim/SpellSim.hpp>

#include <doctest/doctest.h>

#include <filesystem>

using namespace SpellSim;
using Math::Fixed;
using Math::FVec3;

namespace {

const FVec3 look_down{Fixed{}, Fixed::from_int(-1), Fixed{}};

std::vector<std::byte> bytes_of(const char* text) {
    auto program = Runes::parse_program(text);
    REQUIRE(program.has_value());
    return Runes::encode_program(*program);
}

const char* carve_text = "TARGET\nPUSH 3\nCARVE\nHALT\n";

} // namespace

TEST_CASE("SetProgram: encode/decode хеша без потерь, схема читается") {
    const SetProgramCommand set{2, 0xDEADBEEF12345678ULL};
    CHECK(SetProgramCommand::decode(set.encode()).hash == set.hash);
    CHECK(SetProgramCommand::decode(set.encode()).slot == 2);
    Replay::CommandRegistry registry;
    register_commands(registry);
    CHECK(registry.format(set.encode()).starts_with("set_program slot=2 "));
    CHECK(set_program_command(2, set.hash) == set.encode());
}

TEST_CASE("SetProgram: программа из блоба встаёт в слот и работает как обычное заклинание") {
    Simulation sim;
    const auto hash = sim.provide_program(bytes_of(carve_text));
    REQUIRE(hash.has_value());
    CHECK(sim.has_program(*hash));

    const auto before = sim.hashes().at("terrain");
    const Replay::Command set = set_program_command(0, *hash);
    sim.tick(std::span(&set, 1));
    CHECK(sim.programs_set() == 1);
    CHECK(sim.rejected_programs() == 0);

    const Replay::Command cast = cast_command(0, Runes::ManaSource::Personal, look_down);
    sim.tick(std::span(&cast, 1));
    for (int i = 0; i < 4; ++i) sim.tick({});
    CHECK(sim.casts() == 1);
    CHECK(sim.edits_applied() >= 1);
    CHECK(sim.hashes().at("terrain") != before); // земля вырезана
}

TEST_CASE("SetProgram: нет блоба или неверный слот — отказ, слот прежний, симуляция жива") {
    Simulation sim;
    const Replay::Command missing = set_program_command(0, 0x1234);
    sim.tick(std::span(&missing, 1));
    CHECK(sim.rejected_programs() == 1);
    CHECK(sim.programs_set() == 0);

    const auto hash = sim.provide_program(bytes_of(carve_text));
    REQUIRE(hash.has_value());
    const Replay::Command bad_slot = set_program_command(7, *hash);
    sim.tick(std::span(&bad_slot, 1));
    CHECK(sim.rejected_programs() == 2);
}

TEST_CASE("provide_program: мусор отвергается, повтор идемпотентен, лимит числа программ") {
    Simulation sim(Config{.max_custom_programs = 2});
    const std::byte junk[] = {std::byte{1}, std::byte{2}, std::byte{3}};
    CHECK_FALSE(sim.provide_program(junk).has_value());

    const auto a = sim.provide_program(bytes_of(carve_text));
    const auto again = sim.provide_program(bytes_of(carve_text));
    REQUIRE(a.has_value());
    CHECK(*again == *a);
    CHECK(sim.provide_program(bytes_of("PUSH 1\nHALT\n")).has_value());
    CHECK_FALSE(sim.provide_program(bytes_of("PUSH 2\nHALT\n")).has_value()); // третья различная — сверх лимита
}

TEST_CASE("запись и повтор: SetProgram в потоке команд, блоб лежит в файле, хеши совпадают") {
    Replay::CommandRegistry registry;
    register_commands(registry);
    const std::string path = (std::filesystem::temp_directory_path() / "spellsim_set_program.rec").string();
    const auto program = bytes_of(carve_text);

    Replay::StateHashes recorded;
    {
        Simulation sim(Config{.seed = 5});
        auto session = Replay::Session::record(5, path, &registry).value();
        Replay::Driver driver(sim, session);
        for (std::uint32_t t = 0; t < 120; ++t) {
            std::vector<Replay::Command> live;
            if (t == 10) live.push_back(set_program_command(1, driver.submit_blob(program)));
            if (t == 20) live.push_back(cast_command(1, Runes::ManaSource::Personal, look_down));
            REQUIRE(driver.step(live));
        }
        const auto verdict = driver.finish();
        REQUIRE(verdict.has_value());
        recorded = verdict->actual;
        CHECK(sim.programs_set() == 1);
    }

    auto loaded = Replay::Recording::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->blobs.size() == 1);
    CHECK(loaded->find_blob(Math::content_hash(program)) != nullptr);

    Simulation sim(Config{.seed = 5});
    auto replay = Replay::Session::replay(*loaded);
    Replay::Driver driver(sim, replay);
    while (driver.step({})) {}
    const auto verdict = driver.finish();
    REQUIRE(verdict.has_value());
    CHECK(verdict->match);
    CHECK(verdict->actual == recorded);
    CHECK(sim.rejected_programs() == 0);
    std::filesystem::remove(path);
}

TEST_CASE("уровень поверх сида: правки setup и сдвиг появления") {
    Simulation plain;
    Config config;
    config.setup.push_back({.carve = true, .center = Math::WorldPos::from_meters(64, 20, 64), .radius = Fixed::from_int(6)});
    config.spawn_offset_x_m = 10;
    Simulation level(config);
    CHECK(level.hashes().at("terrain") != plain.hashes().at("terrain"));
    const auto px = plain.world().get<Character::Position>(plain.player())->value.x;
    const auto lx = level.world().get<Character::Position>(level.player())->value.x;
    CHECK(lx - px == 10 * Fixed::one_raw);
}

TEST_CASE("blob_reference: только SetProgram ссылается на блоб") {
    CHECK(blob_reference(set_program_command(0, 0xABCDEF0123456789ULL)) == 0xABCDEF0123456789ULL);
    CHECK_FALSE(blob_reference(jump_command()).has_value());
    CHECK_FALSE(blob_reference(cast_command(0, Runes::ManaSource::Personal, look_down)).has_value());
}
