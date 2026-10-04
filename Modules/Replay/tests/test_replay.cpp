#include <Replay/Replay.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace Replay;

namespace {

std::string temp_file(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

/// Хеши трёх подсистем с привычными именами.
StateHashes H(std::uint64_t terrain, std::uint64_t mana, std::uint64_t ecs) {
    StateHashes h;
    h.add("terrain", terrain).add("mana", mana).add("ecs", ecs);
    return h;
}

CommandRegistry game_registry() {
    using F = CommandField;
    CommandRegistry r;
    r.add({1, "move", {F{}, F{"dx", F::Kind::Fixed}, F{}, F{"dz", F::Kind::Fixed}}}).add({2, "jump", {}}).add({3, "cast", {F{"slot", F::Kind::Int}, F{"ax", F::Kind::Fixed}, F{}, F{}}});
    return r;
}

/// Запись из `ticks` тиков, команда каждые 5-й тик; потоковая запись в файл.
void record_file(const std::string& path, std::uint32_t ticks, bool finish) {
    const CommandRegistry registry = game_registry();
    auto session = Session::record(7, path, &registry).value();
    for (std::uint32_t t = 0; t < ticks; ++t) {
        const Command c{.type = static_cast<std::uint16_t>(1 + t % 3), .arg = static_cast<std::int16_t>(t), .x = static_cast<std::int32_t>(t * 100)};
        (void)session.begin_tick(t, t % 5 == 0 ? std::span<const Command>(&c, 1) : std::span<const Command>{});
    }
    if (finish) REQUIRE(session.finish(ticks, H(1, 2, 3)).has_value());
}

constexpr std::size_t wire = 12 + 256, stripe_size = 6 * wire, base = 64; // формат журнала записи: блоки по 256 байт, 4 + 2

void corrupt_block(const std::string& path, std::size_t stripe, std::size_t block) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    const std::streamoff at = static_cast<std::streamoff>(base + stripe * stripe_size + block * wire + 30);
    f.seekg(at);
    char c = 0;
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0xFF);
    f.seekp(at);
    f.write(&c, 1);
}

} // namespace

TEST_CASE("StateHashes: имена, поиск, сравнение по именам, различия, повторное имя") {
    StateHashes a = H(1, 2, 3), b = H(1, 2, 3);
    CHECK(a == b);
    CHECK(a.count() == 3);
    CHECK(a.at("mana") == 2);
    CHECK_FALSE(a.find("nope").has_value());
    CHECK(a.describe() == "terrain=0000000000000001 mana=0000000000000002 ecs=0000000000000003");
    CHECK(a.differing(b).empty());

    const StateHashes c = H(1, 9, 3);
    CHECK_FALSE(a == c);
    const auto names = a.differing(c);
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "mana"); // сразу видно, какая подсистема разошлась

    StateHashes d;
    d.add("terrain", 1).add("mana", 2).add("rng", 4); // другой набор подсистем
    const auto mixed = a.differing(d);
    CHECK(mixed.size() == 2); // ecs нет в d, rng нет в a
    CHECK_FALSE(StateHashes{} == a);
    CHECK(StateHashes{}.empty());
}

TEST_CASE("Recording: команды по тикам и файл туда-обратно") {
    Recording r;
    r.seed = 99;
    r.tick_count = 10;
    r.final_hashes = H(1, 2, 3);
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

TEST_CASE("Recording: чужой файл, пустой и обрезанный дают ошибку") {
    CHECK_FALSE(Recording::load("/nonexistent/flux").has_value());
    const std::string path = temp_file("flux_replay_bad.bin");
    { std::FILE* f = std::fopen(path.c_str(), "wb"); std::fputs("not a replay at all, just text", f); std::fclose(f); }
    CHECK_FALSE(Recording::load(path).has_value());
    { std::FILE* f = std::fopen(path.c_str(), "wb"); std::fclose(f); }
    CHECK_FALSE(Recording::load(path).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("Session: запись и повтор отдают одни и те же команды и сверяют хеши по подсистемам") {
    const std::string path = temp_file("flux_session_test.bin");
    const StateHashes hashes = H(10, 20, 30);
    {
        auto rec = Session::record(5, path).value();
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
    const auto ok = session->finish(20, hashes);
    REQUIRE(ok.has_value());
    CHECK((ok->checked && ok->match));
    CHECK(ok->differing.empty());
    const auto bad = session->finish(20, H(10, 21, 30));
    CHECK_FALSE(bad->match);
    REQUIRE(bad->differing.size() == 1);
    CHECK(bad->differing[0] == "mana");
    const std::string text = describe(*bad, Session::Mode::Replay, 20, 0);
    CHECK(text.find("DIFFER FROM") != std::string::npos);
    CHECK(text.find("mana") != std::string::npos);
    CHECK(describe(*ok, Session::Mode::Replay, 20, 0).find("MATCH") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("Session: аргументы сид и без записи, файл записи не создаётся — ошибка") {
    const std::vector<std::string> args{"--seed", "123"};
    auto s = Session::from_args(args, 1);
    REQUIRE(s.has_value());
    CHECK(s->mode() == Session::Mode::Off);
    CHECK(s->seed() == 123);
    const std::vector<std::string> missing{"--replay", "/nonexistent/x"};
    CHECK_FALSE(Session::from_args(missing, 1).has_value());
    CHECK_FALSE(Session::record(1, "/nonexistent/dir/file.rec").has_value());
    CHECK(Session::record(1).has_value()); // без пути — только в памяти
}

TEST_CASE("FlightRecorder: кольцо хранит последние тики по порядку, дамп с именами подсистем") {
    FlightRecorder fr(4);
    const Command c{.type = 9};
    for (std::uint32_t t = 0; t < 10; ++t) fr.push(t, std::span<const Command>(&c, 1), H(t, 0, 0));
    const auto snap = fr.snapshot();
    REQUIRE(snap.size() == 4);
    CHECK(snap.front().tick == 6);
    CHECK(snap.back().tick == 9);
    CHECK(snap.back().commands[0].type == 9);
    CHECK(snap.back().hashes.at("terrain") == 9);
    const std::string path = temp_file("flux_recorder_test.txt");
    CHECK(fr.dump(path));
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    CHECK(content.find("terrain=0000000000000009") != std::string::npos);
    std::filesystem::remove(path);
}

namespace {
/// Минимальная симуляция: тик считает команды, хеши — две подсистемы.
struct CountingSim {
    std::uint32_t ticks = 0;
    std::uint64_t sum = 0, count = 0;
    void tick(std::span<const Command> commands) {
        for (const Command& c : commands) sum += c.type + 1000ULL * ticks, ++count;
        ++ticks;
    }
    [[nodiscard]] std::uint32_t tick_number() const { return ticks; }
    [[nodiscard]] StateHashes hashes() const {
        StateHashes h;
        h.add("sum", sum).add("count", count);
        return h;
    }
};
static_assert(Simulatable<CountingSim>);
} // namespace

TEST_CASE("Driver: запись и повтор одним вызовом на тик, итог совпадает") {
    const std::string path = temp_file("flux_driver_test.bin");
    StateHashes recorded;
    {
        CountingSim sim;
        Session session = Session::record(3, path).value();
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
    CHECK_FALSE(driver.step({}));
    const auto verdict = driver.finish();
    REQUIRE(verdict.has_value());
    CHECK((verdict->checked && verdict->match));
    CHECK(verdict->actual == recorded);
    CHECK(describe(*verdict, loaded->mode(), 30, 0).find("MATCH") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("CommandRegistry: форматирование по схеме и неизвестные типы") {
    const CommandRegistry r = game_registry();
    CHECK(r.format({.type = 1, .x = 65536, .z = -32768}) == "move dx=1 dz=-0.5");
    CHECK(r.format({.type = 2}) == "jump");
    CHECK(r.format({.type = 3, .arg = 2, .x = 98304}) == "cast slot=2 ax=1.5");
    CHECK(r.format({.type = 9, .arg = 1, .x = 2, .y = 3, .z = 4}) == "type#9 arg=1 x=2 y=3 z=4");
    CHECK(r.find(2) != nullptr);
    CHECK(r.find(7) == nullptr);
}

TEST_CASE("запись самоописываема: схемы в файле, inspect без знания игры") {
    const std::string path = temp_file("flux_schema_test.bin");
    record_file(path, 12, true);
    auto loaded = Recording::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->schemas == game_registry().schemas());
    const std::string text = inspect(*loaded);
    CHECK(text.find("seed 7 | 12 ticks | 3 commands") != std::string::npos);
    CHECK(text.find("final hashes: terrain=0000000000000001 mana=0000000000000002 ecs=0000000000000003") != std::string::npos);
    CHECK(text.find("#1 move dx:fixed dz:fixed") != std::string::npos);
    CHECK(text.find("tick 0: move dx=0 dz=0") != std::string::npos);
    CHECK(inspect(*loaded, 1).find("ещё 2 команд") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("файл записи — журнал с избыточностью: испорченные блоки восстанавливаются при чтении и ремонте") {
    const std::string path = temp_file("flux_recording_damage.rec");
    record_file(path, 600, true);
    const auto clean = Recording::load_with_info(path).value();
    CHECK(clean.second.recovery.clean());
    CHECK(clean.second.complete);
    const Recording original = clean.first;
    REQUIRE(original.command_count() == 120);

    corrupt_block(path, 0, 1);
    corrupt_block(path, 2, 0);
    corrupt_block(path, 2, 4); // два блока в одной полосе — в пределах m = 2
    const auto damaged = Recording::load_with_info(path).value();
    CHECK(damaged.first == original); // запись цела, повтор возможен
    CHECK(damaged.second.recovery.blocks_corrupt == 3);
    CHECK(damaged.second.recovery.blocks_repaired == 3);
    CHECK(describe(damaged.second).find("восстановлено 3") != std::string::npos);
    CHECK(Recording::load(path).has_value());

    const auto fixed = Recording::repair_file(path).value();
    CHECK(fixed.blocks_repaired == 3);
    CHECK(Recording::load_with_info(path)->second.recovery.clean()); // файл переписан: снова целый
    std::filesystem::remove(path);
}

TEST_CASE("слишком сильное повреждение: команды потеряны — повтор отвергается, но inspect показывает, что осталось") {
    const std::string path = temp_file("flux_recording_lost.rec");
    record_file(path, 600, true);
    for (const std::size_t b : {0u, 1u, 2u}) corrupt_block(path, 3, b); // три блока одной полосы при m = 2
    const auto loaded = Recording::load_with_info(path).value();
    CHECK(loaded.second.recovery.records_lost > 0);
    CHECK(loaded.first.command_count() < 120);
    const auto strict = Recording::load(path);
    REQUIRE_FALSE(strict.has_value());
    CHECK(strict.error().find("повтор невозможен") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("аварийное завершение: запись пишется по ходу, оборванный файл читается до последнего сброса") {
    const std::string path = temp_file("flux_recording_crash.rec");
    record_file(path, 400, /*finish=*/false); // сессия уничтожена без finish — как при падении
    const auto loaded = Recording::load_with_info(path).value();
    CHECK_FALSE(loaded.first.complete);
    CHECK_FALSE(loaded.second.complete);
    CHECK(loaded.first.final_hashes.empty());
    CHECK(loaded.first.command_count() == 80); // все команды на месте (деструктор дописал полосу)
    CHECK(loaded.first.tick_count == 396);     // до последней команды
    CHECK(describe(loaded.second).find("оборвана") != std::string::npos);
    CHECK(inspect(loaded.first, 1).find("ЗАПИСЬ ОБОРВАНА") != std::string::npos);

    // Оборванную запись можно проиграть: хеши сверять нечего, но воспроизведение идёт до последней команды.
    CountingSim sim;
    auto session = Session::replay(loaded.first);
    Driver driver(sim, session);
    while (driver.step({})) {}
    const auto verdict = driver.finish().value();
    CHECK_FALSE(verdict.checked);
    CHECK_FALSE(verdict.complete);
    CHECK(describe(verdict, Session::Mode::Replay, 396, 0).find("оборвана") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("старые файлы (версия 1, плоский формат) читаются; хеши получают имена h0…h2") {
    const std::string path = temp_file("flux_legacy.bin");
    {
        std::ofstream out(path, std::ios::binary);
        const std::uint32_t magic = 0x52584C46, ver = 1, count = 1, ticks = 4;
        const std::uint64_t seed = 8, h = 5;
        out.write(reinterpret_cast<const char*>(&magic), 4), out.write(reinterpret_cast<const char*>(&ver), 4);
        out.write(reinterpret_cast<const char*>(&seed), 8), out.write(reinterpret_cast<const char*>(&ticks), 4), out.write(reinterpret_cast<const char*>(&count), 4);
        for (int i = 0; i < 3; ++i) out.write(reinterpret_cast<const char*>(&h), 8);
        const std::uint32_t tick = 1;
        const Command c{.type = 9, .x = 5};
        out.write(reinterpret_cast<const char*>(&tick), 4), out.write(reinterpret_cast<const char*>(&c), sizeof c);
    }
    const auto old = Recording::load_with_info(path).value();
    CHECK(old.second.format == LoadInfo::Format::Legacy);
    CHECK(old.first.schemas.empty());
    CHECK(old.first.final_hashes.at("h0") == 5);
    CHECK(inspect(old.first).find("tick 1: type#9") != std::string::npos);
    CHECK(describe(old.second).find("без избыточности") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("diff: первое расхождение записей, хеши — с названиями подсистем") {
    Recording a;
    a.seed = 1;
    a.tick_count = 10;
    a.final_hashes = H(1, 2, 3);
    a.add(2, {.type = 1, .x = 5});
    a.add(6, {.type = 2});
    Recording b = a;
    CHECK_FALSE(diff(a, b).has_value());

    b.final_hashes = H(1, 2, 4);
    auto d = diff(a, b);
    REQUIRE(d.has_value());
    CHECK(d->kind == Difference::Kind::FinalHash);
    CHECK(d->text.find("ecs") != std::string::npos); // названа подсистема, а не просто «хеши разные»
    CHECK(d->text.find("mana") == std::string::npos);

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

    Recording fewer;
    fewer.seed = 1;
    fewer.tick_count = 10;
    fewer.final_hashes = a.final_hashes;
    fewer.add(2, {.type = 1, .x = 5});
    CHECK(diff(a, fewer)->kind == Difference::Kind::CommandCount);
}

// ---- Блобы: данные, на которые ссылаются команды ----

TEST_CASE("блоб: хеш по содержимому, дубликаты не копятся") {
    Recording r;
    const std::byte data[] = {std::byte{1}, std::byte{2}, std::byte{3}};
    const std::uint64_t h = r.add_blob(data);
    CHECK(h == content_hash(data));
    CHECK(r.add_blob(data) == h);
    CHECK(r.blobs.size() == 1);
    REQUIRE(r.find_blob(h) != nullptr);
    CHECK(r.find_blob(h)->bytes.size() == 3);
    CHECK(r.find_blob(h + 1) == nullptr);
    const std::byte other[] = {std::byte{1}, std::byte{2}};
    CHECK(content_hash(other) != h); // длина входит в хеш
}

TEST_CASE("блоб: save/load и потоковая запись сохраняют блобы и порядок команд") {
    std::vector<std::byte> big(1500); // больше блока журнала (256 байт): запись пересекает границы блоков
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>(i * 7);

    const std::string streamed = temp_file("replay_blob_streamed.rec");
    {
        auto s = Session::record(3, streamed).value();
        const std::uint64_t h = s.add_blob(0, big);
        (void)s.begin_tick(0, std::array{Command{.type = 4, .arg = 1, .x = static_cast<std::int32_t>(h)}});
        (void)s.finish(1, H(1, 2, 3));
    }
    auto loaded = Recording::load(streamed);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->blobs.size() == 1);
    CHECK(loaded->blobs[0].bytes == big);
    CHECK(loaded->command_count() == 1);

    const std::string saved = temp_file("replay_blob_saved.rec");
    REQUIRE(loaded->save(saved).has_value());
    auto again = Recording::load(saved);
    REQUIRE(again.has_value());
    CHECK(*again == *loaded);
    std::filesystem::remove(streamed);
    std::filesystem::remove(saved);
}

TEST_CASE("блоб: испорченное содержимое отбрасывается по хешу, а не подменяется") {
    const std::string path = temp_file("replay_blob_corrupt.rec");
    Recording r;
    r.seed = 1;
    r.tick_count = 1;
    const std::byte data[] = {std::byte{9}, std::byte{8}, std::byte{7}};
    r.add_blob(data);
    r.blobs[0].bytes[1] = std::byte{0}; // хеш не сходится с содержимым
    REQUIRE(r.save(path).has_value());
    auto loaded = Recording::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->blobs.empty());
    std::filesystem::remove(path);
}
