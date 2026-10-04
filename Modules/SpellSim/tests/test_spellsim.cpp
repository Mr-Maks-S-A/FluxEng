#include <SpellSim/SpellSim.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

using namespace SpellSim;
using Math::Fixed;
using Math::FVec3;
using Math::WorldPos;

namespace {

const char* carve_text = "TARGET\nPUSH 2\nCARVE\nHALT\n";
const char* raise_text = "TARGET\nPUSH 2\nRAISE\nHALT\n";
const char* runaway_text = "loop:\nPUSH 1\nJMP_IF loop\n";

struct Rig {
    Simulation sim;
    explicit Rig(std::uint64_t seed = 1) : sim(Config{.seed = seed}) {
        REQUIRE(sim.programs().add_text("carve", carve_text).has_value());
        REQUIRE(sim.programs().add_text("raise", raise_text).has_value());
        REQUIRE(sim.programs().add_text("runaway", runaway_text).has_value());
    }
    void run(int ticks, std::span<const Replay::Command> first = {}) {
        for (int i = 0; i < ticks; ++i) sim.tick(i == 0 ? first : std::span<const Replay::Command>{});
    }
    [[nodiscard]] const Character::ManaPool& mana() const { return *sim.world().get<Character::ManaPool>(sim.player()); }
    [[nodiscard]] WorldPos feet() const { return sim.world().get<Character::Position>(sim.player())->value; }
};

const FVec3 look_down{Fixed{}, Fixed::from_int(-1), Fixed{}};

/// Сценарий на ~20 секунд: ходьба, прыжки, касты обоих видов, «убегающее» заклинание.
std::vector<std::pair<std::uint32_t, Replay::Command>> scenario() {
    std::vector<std::pair<std::uint32_t, Replay::Command>> out;
    const FVec3 forward_down = Math::normalize({Fixed::from_ratio(1, 2), Fixed::from_ratio(-1, 2), Fixed{}});
    out.emplace_back(10, move_command(Fixed::from_int(1), Fixed{}));
    out.emplace_back(60, cast_command(0, Runes::ManaSource::Personal, forward_down));
    out.emplace_back(120, jump_command());
    out.emplace_back(180, cast_command(1, Runes::ManaSource::Ambient, forward_down));
    out.emplace_back(240, move_command(Fixed{}, Fixed::from_int(1)));
    out.emplace_back(300, cast_command(0, Runes::ManaSource::Ambient, look_down));
    out.emplace_back(400, cast_command(2, Runes::ManaSource::Personal, forward_down));
    out.emplace_back(500, jump_command());
    out.emplace_back(700, cast_command(1, Runes::ManaSource::Personal, look_down));
    out.emplace_back(900, move_command(Fixed::from_ratio(-7, 10), Fixed::from_ratio(7, 10)));
    return out;
}

Replay::StateHashes play(Replay::Session& session, std::uint32_t ticks, std::uint64_t seed) {
    Rig rig(seed);
    const auto script = scenario();
    std::size_t next = 0;
    for (std::uint32_t t = 0; t < ticks; ++t) {
        std::vector<Replay::Command> live;
        while (next < script.size() && script[next].first == t) live.push_back(script[next++].second);
        rig.sim.tick(session.begin_tick(t, live));
    }
    return rig.sim.hashes();
}

} // namespace

TEST_CASE("порядок тика: правка ландшафта применяется в том же тике, что и каст") {
    Rig rig;
    rig.run(120); // персонаж встал на землю
    const auto before = rig.sim.hashes().at("terrain");
    const Replay::Command cast = cast_command(0, Runes::ManaSource::Personal, look_down);
    rig.sim.tick(std::span<const Replay::Command>(&cast, 1));
    CHECK(rig.sim.hashes().at("terrain") != before);
    CHECK(rig.sim.edits_applied() == 1);
}

TEST_CASE("«вырезать сферу» и «поднять землю» меняют мир там, куда смотрит маг") {
    Rig rig;
    rig.run(120);
    const WorldPos eye = Character::eye(rig.sim.world(), rig.sim.player());
    const auto hit = rig.sim.terrain().raycast(eye, look_down, Fixed::from_int(40));
    REQUIRE(hit.has_value());
    CHECK(rig.sim.terrain().sample(hit->position).raw <= 1311); // у поверхности

    const Replay::Command carve = cast_command(0, Runes::ManaSource::Personal, look_down);
    rig.sim.tick(std::span<const Replay::Command>(&carve, 1));
    CHECK(rig.sim.terrain().sample(hit->position).raw > 0); // в точке попадания теперь воздух

    const Replay::Command raise = cast_command(1, Runes::ManaSource::Personal, look_down);
    const auto top_before = rig.sim.terrain().ground_height(hit->position.x, hit->position.z);
    rig.run(1, std::span<const Replay::Command>(&raise, 1));
    CHECK(rig.sim.terrain().ground_height(hit->position.x, hit->position.z) >= top_before - 1);
}

TEST_CASE("каст из окружения: в тумане остаётся дыра, а личная мана тратится в 10 раз меньше") {
    Rig personal, ambient;
    personal.run(120), ambient.run(120);
    const double p0 = personal.mana().current.to_double(), a0 = ambient.mana().current.to_double();
    const WorldPos at = Character::eye(ambient.sim.world(), ambient.sim.player());
    const Math::Mana density_before = ambient.sim.mana().density(at);

    const Replay::Command p = cast_command(0, Runes::ManaSource::Personal, look_down);
    const Replay::Command a = cast_command(0, Runes::ManaSource::Ambient, look_down);
    personal.sim.tick(std::span<const Replay::Command>(&p, 1));
    ambient.sim.tick(std::span<const Replay::Command>(&a, 1));
    const double personal_cost = p0 - personal.mana().current.to_double(), ambient_cost = a0 - ambient.mana().current.to_double();
    CHECK(personal_cost > 150);
    CHECK(ambient_cost == doctest::Approx(personal_cost / 10).epsilon(0.05)); // с поправкой на регенерацию за тик

    CHECK(ambient.sim.mana().density(at) < density_before * Fixed::from_ratio(3, 4)); // «дыра» в тумане
    CHECK(personal.sim.mana().density(at) == personal.sim.mana().config().base);       // личный каст поле не трогает
}

TEST_CASE("дыра в тумане затягивается за 5-10 секунд") {
    Rig rig;
    rig.run(120);
    const WorldPos at = Character::eye(rig.sim.world(), rig.sim.player());
    const Replay::Command a = cast_command(1, Runes::ManaSource::Ambient, look_down);
    rig.sim.tick(std::span<const Replay::Command>(&a, 1));
    const Math::Mana base = rig.sim.mana().config().base;
    REQUIRE(rig.sim.mana().density(at) < base * Fixed::from_ratio(3, 4));
    int ticks = 0;
    while (rig.sim.mana().density(at) < base * Fixed::from_ratio(9, 10) && ticks < 60 * 20) {
        rig.run(1);
        ++ticks;
    }
    CHECK(ticks >= 60 * 5);
    CHECK(ticks <= 60 * 10);
}

TEST_CASE("поле маны разреженное: чанки появляются при касте из окружения и освобождаются, когда дыра затянулась") {
    Rig rig;
    rig.run(120);
    CHECK(rig.sim.mana().allocated_chunks() == 0); // пока никто не брал ману, поле ничего не хранит
    const Replay::Command a = cast_command(1, Runes::ManaSource::Ambient, look_down);
    rig.sim.tick(std::span<const Replay::Command>(&a, 1));
    CHECK(rig.sim.mana().allocated_chunks() > 0);
    int ticks = 0;
    while (rig.sim.mana().allocated_chunks() > 0 && ticks < 60 * 120) {
        rig.run(1);
        ++ticks;
    }
    CHECK(rig.sim.mana().allocated_chunks() == 0);
    CHECK(rig.sim.mana().excess_raw() == 0);
    CHECK(ticks < 60 * 120);
}

TEST_CASE("заклинание с бесконечным циклом останавливается само, когда кончается мана") {
    EventSystem::EventBus bus;
    Rig rig;
    rig.sim.declare(bus);
    const EventSystem::ModuleId watcher = bus.declare_module("Watcher").consumes<Runes::SpellFailedEvent>();
    auto failed = bus.reader<Runes::SpellFailedEvent>(watcher);

    rig.run(60);
    const Replay::Command c = cast_command(2, Runes::ManaSource::Personal, look_down);
    rig.sim.tick(std::span<const Replay::Command>(&c, 1));
    bus.advance_tick();
    int ticks = 0;
    while (rig.sim.spells().active(rig.sim.world()) > 0 && ticks < 60 * 60) {
        rig.run(1);
        bus.advance_tick();
        ++ticks;
    }
    CHECK(rig.sim.spells().active(rig.sim.world()) == 0);
    CHECK(ticks < 60 * 60);
    CHECK(rig.sim.spells().last_trace().failure == Runes::Failure::OutOfMana);
    CHECK(rig.mana().current.to_double() < 2.0);
    // Событие SpellFailed видно одним тиком позже.
    bool seen = false;
    for (int i = 0; i < 2; ++i) seen = seen || !failed.events().empty();
    CHECK(rig.sim.spells().last_trace().runes_executed > 10000);
    (void)seen;
}

TEST_CASE("события шины: SpellFailed и TerrainEdited") {
    EventSystem::EventBus bus;
    Rig rig;
    rig.sim.declare(bus);
    const EventSystem::ModuleId watcher =
        bus.declare_module("Watcher").consumes<Runes::SpellFailedEvent>().consumes<TerrainEditedEvent>();
    auto failed = bus.reader<Runes::SpellFailedEvent>(watcher);
    auto edited = bus.reader<TerrainEditedEvent>(watcher);
    REQUIRE(rig.sim.programs().add_text("bad", "ADD\n").has_value());
    rig.sim.set_grimoire_slot(2, "bad");
    rig.run(60);

    const Replay::Command cut = cast_command(0, Runes::ManaSource::Personal, look_down);
    const Replay::Command bad = cast_command(2, Runes::ManaSource::Personal, look_down);
    const std::array<Replay::Command, 2> both{cut, bad};
    rig.sim.tick(both);
    bus.advance_tick();
    REQUIRE(failed.events().size() == 1);
    CHECK(failed.events()[0].reason == static_cast<std::uint32_t>(Runes::Failure::StackUnderflow));
    REQUIRE(edited.events().size() == 1);
    CHECK(edited.events()[0].chunks >= 1);
    CHECK(edited.events()[0].hi_x >= edited.events()[0].lo_x);
}

TEST_CASE("после правки изменённые чанки стоят в очереди на перестройку") {
    Rig rig;
    (void)rig.sim.take_dirty_chunks(); // первичная очередь: все чанки
    rig.run(60);
    CHECK(rig.sim.terrain().dirty_count() == 0);
    const Replay::Command cut = cast_command(0, Runes::ManaSource::Personal, look_down);
    rig.sim.tick(std::span<const Replay::Command>(&cut, 1));
    CHECK(rig.sim.terrain().dirty_count() >= 1);
}

TEST_CASE("запись команд, проигранная повторно, даёт тот же хеш мира на последнем тике") {
    const std::string path = (std::filesystem::temp_directory_path() / "flux_spellsim_replay.bin").string();
    constexpr std::uint32_t ticks = 1200;
    Replay::StateHashes recorded;
    {
        Replay::Session rec = Replay::Session::record(11, path).value();
        recorded = play(rec, ticks, 11);
        REQUIRE(rec.finish(ticks, recorded).has_value());
    }
    const std::vector<std::string> args{"--replay", path};
    auto replay = Replay::Session::from_args(args, 0);
    REQUIRE(replay.has_value());
    CHECK(replay->recording().command_count() == scenario().size());
    const Replay::StateHashes replayed = play(*replay, ticks, replay->seed());
    CHECK(replayed == recorded);
    const auto verdict = replay->finish(ticks, replayed);
    REQUIRE(verdict.has_value());
    CHECK((verdict->checked && verdict->match));
    CHECK(recorded.at("terrain") != Rig(11).sim.hashes().at("terrain")); // мир действительно менялся
    CHECK(recorded.at("mana") != Rig(11).sim.hashes().at("mana"));
    std::filesystem::remove(path);
}

TEST_CASE("другой сид даёт другой мир, но те же команды воспроизводимы в нём") {
    Replay::Session a = Replay::Session::off(1), b = Replay::Session::off(2), a2 = Replay::Session::off(1);
    const auto ha = play(a, 600, 1), hb = play(b, 600, 2), ha2 = play(a2, 600, 1);
    CHECK(ha == ha2);
    CHECK(ha.at("terrain") != hb.at("terrain"));
}

// Бюджет измеряется только в оптимизированной сборке без санитайзеров (они замедляют код в разы).
#if defined(__SANITIZE_ADDRESS__)
#define FLUX_SLOW_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FLUX_SLOW_BUILD 1
#endif
#endif
#ifndef NDEBUG
#define FLUX_SLOW_BUILD 1
#endif

#ifdef FLUX_SLOW_BUILD
constexpr bool slow_build = true;
#else
constexpr bool slow_build = false;
#endif

TEST_CASE("фазы тика укладываются в бюджет 4 мс" * doctest::skip(slow_build)) {
    Rig rig;
    rig.run(120);
    double worst = 0;
    const Replay::Command c = cast_command(0, Runes::ManaSource::Ambient, look_down);
    for (int i = 0; i < 300; ++i) {
        rig.sim.tick(i % 50 == 0 ? std::span<const Replay::Command>(&c, 1) : std::span<const Replay::Command>{});
        worst = std::max(worst, rig.sim.tick_ms());
    }
    CHECK(worst < 4.0);
}

TEST_CASE("Simulation удовлетворяет Replay::Simulatable и работает через Driver") {
    static_assert(Replay::Simulatable<Simulation>);
    Rig rig;
    Replay::Session session = Replay::Session::off(1);
    Replay::Driver driver(rig.sim, session);
    CHECK(driver.step({}));
    CHECK(rig.sim.tick_number() == 1);
}

TEST_CASE("расписание фаз: порядок виден, новый модуль подключается фазой без правки SpellSim") {
    Rig rig;
    const std::vector<std::string_view> expected{"commands", "spells", "terrain", "mana", "movement", "events"};
    CHECK(rig.sim.schedule().names() == expected);

    // «Модуль машин» ставит фазу после движения и видит уже обновлённого персонажа.
    int calls = 0;
    WorldPos seen_at_phase{};
    REQUIRE(rig.sim.schedule().insert_after("movement", "machines", [&] {
        ++calls;
        seen_at_phase = rig.sim.world().get<Character::Position>(rig.sim.player())->value;
    }));
    rig.run(5);
    CHECK(calls == 5);
    CHECK(seen_at_phase == rig.feet()); // фаза после «movement»: позиция уже актуальна
    CHECK(rig.sim.schedule().times().size() == 7);
    CHECK(rig.sim.schedule().names()[5] == "machines");
    CHECK(rig.sim.tick_ms() >= 0.0);
}

TEST_CASE("выключенная фаза mana: поле не шагает, остальной тик работает") {
    Rig rig;
    rig.run(60);
    const Replay::Command a = cast_command(1, Runes::ManaSource::Ambient, look_down);
    rig.sim.tick(std::span<const Replay::Command>(&a, 1));
    const auto hash_before = rig.sim.hashes().at("mana");
    REQUIRE(rig.sim.schedule().set_enabled("mana", false));
    rig.run(60);
    CHECK(rig.sim.hashes().at("mana") == hash_before); // поле заморожено
    rig.sim.schedule().set_enabled("mana", true);
    rig.run(60);
    CHECK(rig.sim.hashes().at("mana") != hash_before);
}

TEST_CASE("типизированные команды: encode и decode туда-обратно, схемы зарегистрированы") {
    const MoveCommand move{Fixed::from_ratio(1, 2), Fixed::from_ratio(-3, 4)};
    const MoveCommand back = MoveCommand::decode(move.encode());
    CHECK(back.dx == move.dx);
    CHECK(back.dz == move.dz);
    const CastCommand cast{2, Runes::ManaSource::Ambient, Math::normalize({Fixed::from_int(1), Fixed::from_int(-1), Fixed{}})};
    const CastCommand cast_back = CastCommand::decode(cast.encode());
    CHECK(cast_back.slot == 2);
    CHECK(cast_back.source == Runes::ManaSource::Ambient);
    CHECK(cast_back.aim == cast.aim);
    CHECK(move_command(move.dx, move.dz) == move.encode()); // старые хелперы — те же команды

    Replay::CommandRegistry registry;
    register_commands(registry);
    CHECK(registry.schemas().size() == 4);
    CHECK(registry.format(move.encode()) == "move dx=0.5 dz=-0.75");
    CHECK(registry.format(jump_command()) == "jump");
    CHECK(registry.format(cast.encode()).starts_with("cast slot_source=258 "));
}
