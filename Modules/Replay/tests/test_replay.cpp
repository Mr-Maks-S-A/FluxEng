#include <Replay/Replay.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace Replay;

namespace {
std::string temp_file(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }
} // namespace

TEST_CASE("Recording: команды по тикам и файл туда-обратно") {
    Recording r;
    r.seed = 99;
    r.tick_count = 10;
    r.final_hashes = {{1, 2, 3}};
    r.add(2, {.type = 1, .arg = 5, .x = -7, .y = 0, .z = 9});
    r.add(2, {.type = 2});
    r.add(7, {.type = 3, .x = 1});
    CHECK(r.at(2).size() == 2);
    CHECK(r.at(3).empty());
    CHECK(r.at(7)[0].type == 3);

    const std::string path = temp_file("flux_replay_test.bin");
    REQUIRE(r.save(path).has_value());
    auto loaded = Recording::load(path);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == r);
    std::filesystem::remove(path);
}

TEST_CASE("Recording: чужой и обрезанный файл дают ошибку") {
    CHECK_FALSE(Recording::load("/nonexistent/flux").has_value());
    const std::string path = temp_file("flux_replay_bad.bin");
    { std::FILE* f = std::fopen(path.c_str(), "wb"); std::fputs("not a replay", f); std::fclose(f); }
    CHECK_FALSE(Recording::load(path).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("Session: запись и повтор отдают одни и те же команды и сверяют хеши") {
    const std::string path = temp_file("flux_session_test.bin");
    const StateHashes hashes{{10, 20, 30}};
    {
        Session rec = Session::record(5, path);
        const Command a{.type = 1, .x = 4};
        for (std::uint32_t t = 0; t < 20; ++t) {
            const std::span<const Command> live = (t % 5 == 0) ? std::span<const Command>(&a, 1) : std::span<const Command>{};
            CHECK(rec.begin_tick(t, live).size() == live.size());
        }
        REQUIRE(rec.finish(20, hashes).has_value());
    }
    const std::vector<std::string> args{"--replay", path};
    auto session = Session::from_args(args, 0);
    REQUIRE(session.has_value());
    CHECK(session->mode() == Session::Mode::Replay);
    CHECK(session->seed() == 5);
    std::size_t total = 0;
    for (std::uint32_t t = 0; !session->finished(t); ++t) total += session->begin_tick(t, {}).size();
    CHECK(total == 4);
    auto ok = session->finish(20, hashes);
    REQUIRE(ok.has_value());
    CHECK((ok->checked && ok->match));
    auto bad = session->finish(20, StateHashes{{10, 20, 31}});
    CHECK_FALSE(bad->match);
    std::filesystem::remove(path);
}

TEST_CASE("Session: аргументы сид и без записи") {
    const std::vector<std::string> args{"--seed", "123"};
    auto s = Session::from_args(args, 1);
    REQUIRE(s.has_value());
    CHECK(s->mode() == Session::Mode::Off);
    CHECK(s->seed() == 123);
    const std::vector<std::string> missing{"--replay", "/nonexistent/x"};
    CHECK_FALSE(Session::from_args(missing, 1).has_value());
}

TEST_CASE("FlightRecorder: кольцо хранит последние тики по порядку") {
    FlightRecorder fr(4);
    const Command c{.type = 9};
    for (std::uint32_t t = 0; t < 10; ++t) fr.push(t, std::span<const Command>(&c, 1), {{t, 0, 0}});
    const auto snap = fr.snapshot();
    REQUIRE(snap.size() == 4);
    CHECK(snap.front().tick == 6);
    CHECK(snap.back().tick == 9);
    CHECK(snap.back().commands[0].type == 9);
    const std::string path = temp_file("flux_recorder_test.txt");
    CHECK(fr.dump(path));
    CHECK(std::filesystem::file_size(path) > 0);
    std::filesystem::remove(path);
}

namespace {
/// Минимальная симуляция: тик считает команды, хеш — сумма типов команд.
struct CountingSim {
    std::uint32_t ticks = 0;
    std::uint64_t sum = 0;
    void tick(std::span<const Command> commands) {
        for (const Command& c : commands) sum += c.type + 1000ULL * ticks;
        ++ticks;
    }
    [[nodiscard]] std::uint32_t tick_number() const { return ticks; }
    [[nodiscard]] StateHashes hashes() const { return {{sum, ticks, 7}}; }
};
static_assert(Simulatable<CountingSim>);
} // namespace

TEST_CASE("Driver: запись и повтор одним вызовом на тик, итог совпадает") {
    const std::string path = temp_file("flux_driver_test.bin");
    StateHashes recorded;
    {
        CountingSim sim;
        Session session = Session::record(3, path);
        FlightRecorder fr(8);
        Driver driver(sim, session, &fr);
        for (std::uint32_t t = 0; t < 30; ++t) {
            const Command c{.type = static_cast<std::uint16_t>(t % 4)};
            std::span<const Command> live = (t % 3 == 0) ? std::span<const Command>(&c, 1) : std::span<const Command>{};
            REQUIRE(driver.step(live));
        }
        CHECK(fr.snapshot().size() == 8);
        auto verdict = driver.finish();
        REQUIRE(verdict.has_value());
        recorded = verdict->actual;
        CHECK(describe(*verdict, session.mode(), 30, session.recording().command_count()).starts_with("recorded 30 ticks"));
    }
    CountingSim sim;
    auto loaded = Session::from_args(std::vector<std::string>{"--replay", path}, 0);
    REQUIRE(loaded.has_value());
    Driver driver(sim, *loaded);
    CHECK(driver.replaying());
    std::uint32_t steps = 0;
    while (driver.step({})) ++steps; // ввод не нужен: команды берутся из записи
    CHECK(steps == 30);
    CHECK_FALSE(driver.step({})); // после конца записи тики не выполняются
    const auto verdict = driver.finish();
    REQUIRE(verdict.has_value());
    CHECK((verdict->checked && verdict->match));
    CHECK(verdict->actual == recorded);
    CHECK(describe(*verdict, loaded->mode(), 30, 0).find("MATCH") != std::string::npos);
    std::filesystem::remove(path);
}

namespace {
CommandRegistry game_registry() {
    using F = CommandField;
    CommandRegistry r;
    r.add({1, "move", {F{}, F{"dx", F::Kind::Fixed}, F{}, F{"dz", F::Kind::Fixed}}}).add({2, "jump", {}}).add({3, "cast", {F{"slot", F::Kind::Int}, F{"ax", F::Kind::Fixed}, F{}, F{}}});
    return r;
}
} // namespace

TEST_CASE("CommandRegistry: форматирование по схеме и неизвестные типы") {
    const CommandRegistry r = game_registry();
    CHECK(r.format({.type = 1, .x = 65536, .z = -32768}) == "move dx=1 dz=-0.5");
    CHECK(r.format({.type = 2}) == "jump");
    CHECK(r.format({.type = 3, .arg = 2, .x = 98304}) == "cast slot=2 ax=1.5");
    CHECK(r.format({.type = 9, .arg = 1, .x = 2, .y = 3, .z = 4}) == "type#9 arg=1 x=2 y=3 z=4");
    CHECK(r.find(2) != nullptr);
    CHECK(r.find(7) == nullptr);
}

TEST_CASE("запись самоописываема: схемы в файле, inspect без знания игры, чтение старого формата") {
    const std::string path = temp_file("flux_schema_test.bin");
    const CommandRegistry registry = game_registry();
    {
        Session rec = Session::record(5, path, &registry);
        const Command move{.type = 1, .x = 65536};
        const Command jump{.type = 2};
        (void)rec.begin_tick(3, std::span<const Command>(&move, 1));
        (void)rec.begin_tick(7, std::span<const Command>(&jump, 1));
        REQUIRE(rec.finish(10, {{1, 2, 3}}).has_value());
    }
    auto loaded = Recording::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->schemas == registry.schemas());
    const std::string text = inspect(*loaded);
    CHECK(text.find("seed 5 | 10 ticks | 2 commands") != std::string::npos);
    CHECK(text.find("#1 move dx:fixed dz:fixed") != std::string::npos);
    CHECK(text.find("tick 3: move dx=1 dz=0") != std::string::npos);
    CHECK(text.find("tick 7: jump") != std::string::npos);
    CHECK(inspect(*loaded, 1).find("ещё 1 команд") != std::string::npos);

    // Файл версии 1 (без таблицы схем) читается: схем нет, команды печатаются сырыми.
    {
        std::ofstream out(path, std::ios::binary);
        const std::uint32_t magic = 0x52584C46, ver = 1, count = 1, ticks = 4;
        const std::uint64_t seed = 8, h = 0;
        out.write(reinterpret_cast<const char*>(&magic), 4), out.write(reinterpret_cast<const char*>(&ver), 4);
        out.write(reinterpret_cast<const char*>(&seed), 8), out.write(reinterpret_cast<const char*>(&ticks), 4), out.write(reinterpret_cast<const char*>(&count), 4);
        for (int i = 0; i < 3; ++i) out.write(reinterpret_cast<const char*>(&h), 8);
        const std::uint32_t tick = 1;
        const Command c{.type = 9, .x = 5};
        out.write(reinterpret_cast<const char*>(&tick), 4), out.write(reinterpret_cast<const char*>(&c), sizeof c);
    }
    auto old = Recording::load(path);
    REQUIRE(old.has_value());
    CHECK(old->schemas.empty());
    CHECK(inspect(*old).find("tick 1: type#9") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("diff: первое расхождение записей") {
    Recording a;
    a.seed = 1;
    a.tick_count = 10;
    a.final_hashes = {{1, 2, 3}};
    a.add(2, {.type = 1, .x = 5});
    a.add(6, {.type = 2});
    Recording b = a;
    CHECK_FALSE(diff(a, b).has_value());

    b.final_hashes = {{1, 2, 4}};
    auto d = diff(a, b);
    REQUIRE(d.has_value());
    CHECK(d->kind == Difference::Kind::FinalHash);

    Recording seed = a;
    seed.seed = 2;
    CHECK(diff(a, seed)->kind == Difference::Kind::Seed);

    Recording length = a;
    length.tick_count = 11;
    CHECK(diff(a, length)->kind == Difference::Kind::TickCount);

    Recording other;
    other.seed = 1;
    other.tick_count = 10;
    other.final_hashes = a.final_hashes;
    other.add(2, {.type = 1, .x = 6}); // другая команда на том же тике
    other.add(6, {.type = 2});
    const auto changed = diff(a, other);
    REQUIRE(changed.has_value());
    CHECK(changed->kind == Difference::Kind::Command);
    CHECK(changed->tick == 2);

    Recording fewer = a;
    fewer = Recording{};
    fewer.seed = 1;
    fewer.tick_count = 10;
    fewer.final_hashes = a.final_hashes;
    fewer.add(2, {.type = 1, .x = 5});
    CHECK(diff(a, fewer)->kind == Difference::Kind::CommandCount);
}
