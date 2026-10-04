/**
 * @file test_net_spellsim.cpp
 * @brief Net + SpellSim + Challenge + Replay: сетевая партия «пилот и писарь» — правки заклинаний идут в потоке команд.
 *
 * Два игрока играют один уровень: пилот ходит и кастует, писарь пишет заклинание (блоб + `SetProgram`). У каждого свой мир,
 * по сети идёт только ввод. Проверяется то, ради чего всё затеяно: при любом качестве сети миры совпадают побитно, а
 * сетевую партию можно записать и воспроизвести тем же файлом.
 */

#include <Challenge/Play.hpp>
#include <Net/Lockstep.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>

namespace {

/// Участник партии: мир, сетевой узел и (по желанию) запись объединённого ввода.
struct Peer {
    Peer(const Challenge::Level& level, Net::Transport& transport, Net::PeerId id)
        : sim(level.config), net(transport, {.local = id, .peers = 2, .seed = level.config.seed, .config_hash = Challenge::level_hash(level), .input_delay = 3}) {
        Challenge::install_library(level, sim);
        net.set_blob_reference(SpellSim::blob_reference);
    }
    SpellSim::Simulation sim;
    Net::Lockstep net;
    std::unique_ptr<Replay::Session> record; ///< Запись партии с точки зрения этого пира.
    std::uint32_t cap = ~0u; ///< Не шагать дальше этого тика (но продолжать слать и принимать).

    /// Один кадр. `mine` — ввод этого пира на тик, который сейчас подаётся.
    void frame(const std::function<void(const SpellSim::Simulation&, std::vector<Replay::Command>&)>& input) {
        net.pump();
        if (net.needs_input()) {
            std::vector<Replay::Command> mine;
            input(sim, mine);
            net.submit(mine);
        }
        if (sim.tick_number() >= cap || !net.ready()) return;
        const auto span = net.advance();
        const std::vector<Replay::Command> commands(span.begin(), span.end());
        for (const Replay::Command& c : commands) {
            const auto hash = SpellSim::blob_reference(c);
            if (!hash) continue;
            if (!sim.has_program(*hash)) (void)sim.provide_program(*net.blob(*hash));
            if (record) (void)record->add_blob(sim.tick_number(), *net.blob(*hash)); // блоб — в запись раньше команды
        }
        const std::span<const Replay::Command> applied = record ? record->begin_tick(sim.tick_number(), commands) : std::span<const Replay::Command>(commands);
        sim.tick(applied);
        net.report_hashes(sim.hashes());
    }
};

struct Party {
    std::unique_ptr<Net::LoopbackNetwork> network;
    const Challenge::Level& level;
    const Challenge::Solution& solution;
    std::unique_ptr<Peer> pilot, scribe;
    bool scribe_wrote = false;

    Party(const char* level_id, const Net::LinkConfig& link, std::uint64_t seed)
        : network(std::make_unique<Net::LoopbackNetwork>(2, link, seed)), level(*Challenge::find_level(level_id)), solution(*Challenge::reference_solution(level_id)) {
        pilot = std::make_unique<Peer>(level, network->endpoint(0), 0);
        scribe = std::make_unique<Peer>(level, network->endpoint(1), 1);
    }

    void frame() {
        pilot->frame([&](const SpellSim::Simulation& sim, std::vector<Replay::Command>& out) {
            if (solution.policy) solution.policy(sim, out);
        });
        scribe->frame([&](const SpellSim::Simulation&, std::vector<Replay::Command>& out) {
            if (scribe_wrote) return;
            scribe_wrote = true;
            for (const auto& [slot, text] : solution.programs) {
                const auto program = Runes::parse_program(text);
                REQUIRE(program.has_value());
                out.push_back(SpellSim::set_program_command(slot, scribe->net.submit_blob(Runes::encode_program(*program))));
            }
        });
        network->step();
    }

    /// Играет, пока оба мира не дойдут до тика `ticks` (дальше не шагают, но продолжают слать).
    void run_to(std::uint32_t ticks) {
        pilot->cap = scribe->cap = ticks;
        for (int i = 0; i < 200000 && !(pilot->sim.tick_number() >= ticks && scribe->sim.tick_number() >= ticks); ++i) {
            frame();
        }
    }
};

} // namespace

TEST_CASE("cpu: сетевая партия «пилот и писарь»: миры совпадают побитно при идеальной, плохой и ужасной сети") {
    struct Case { const char* level; Net::LinkConfig link; std::uint32_t ticks; };
    const Case cases[] = {
        {"mound", {}, 120},
        {"mound", {.latency_steps = 2, .jitter_steps = 3, .loss_permille = 150, .duplicate_permille = 50, .corrupt_permille = 20}, 120},
        {"mound", {.latency_steps = 6, .jitter_steps = 8, .loss_permille = 300, .duplicate_permille = 150, .corrupt_permille = 80}, 120},
        {"fort", {.latency_steps = 3, .jitter_steps = 4, .loss_permille = 200, .duplicate_permille = 100, .corrupt_permille = 50}, 500},
    };
    for (const Case& c : cases) {
        CAPTURE(c.level);
        CAPTURE(c.link.loss_permille);
        Party party(c.level, c.link, 5);
        // Выполняется ровно `ticks` тиков у обоих — состояния сравнимы тик в тик.
        party.run_to(c.ticks);
        REQUIRE(party.pilot->sim.tick_number() == party.scribe->sim.tick_number());
        CHECK(party.pilot->sim.hashes() == party.scribe->sim.hashes());
        CHECK_FALSE(party.pilot->net.desync().has_value());
        CHECK_FALSE(party.scribe->net.desync().has_value());
        CHECK(party.pilot->sim.programs_set() == party.solution.programs.size());
        CHECK(party.pilot->sim.rejected_programs() == 0);
        CHECK(party.pilot->sim.casts() >= 1); // партия действительно что-то наколдовала
    }
}

TEST_CASE("cpu: одна и та же партия в любой сети даёт один и тот же мир (сеть меняет время, а не результат)") {
    Party ideal("mound", {}, 1), bad("mound", {.latency_steps = 4, .jitter_steps = 5, .loss_permille = 250}, 9);
    ideal.run_to(100);
    bad.run_to(100);
    CHECK(ideal.pilot->sim.hashes() == bad.pilot->sim.hashes());
    CHECK(ideal.pilot->sim.hashes() == bad.scribe->sim.hashes());
}

TEST_CASE("cpu: сетевую партию можно записать и воспроизвести из файла: блоб программы лежит в записи, хеши совпадают") {
    const std::string path = (std::filesystem::temp_directory_path() / "itest_net_party.rec").string();
    Replay::CommandRegistry registry;
    SpellSim::register_commands(registry);

    Party party("mound", {.latency_steps = 2, .jitter_steps = 3, .loss_permille = 150}, 3);
    party.pilot->record = std::make_unique<Replay::Session>(Replay::Session::record(party.level.config.seed, path, &registry).value());
    party.run_to(150);
    REQUIRE(party.pilot->sim.tick_number() == 150);
    const Replay::StateHashes live = party.pilot->sim.hashes();
    REQUIRE(party.pilot->record->finish(150, live).has_value());
    party.pilot->record.reset();

    auto loaded = Replay::Recording::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->blobs.size() == 1); // программа писаря

    SpellSim::Simulation replayed(party.level.config);
    Challenge::install_library(party.level, replayed);
    auto session = Replay::Session::replay(*loaded);
    Replay::Driver driver(replayed, session);
    while (driver.step({})) {}
    const auto verdict = driver.finish();
    REQUIRE(verdict.has_value());
    CHECK(verdict->match);
    CHECK(replayed.hashes() == live);
    std::filesystem::remove(path);
}

TEST_CASE("cpu: команда со ссылкой на блоб, которого нет, не исполняется: мир ждёт, а не делает что попало") {
    // Пир шлёт SetProgram с хешем, но блоб никто не присылал: оба мира останавливаются на тике команды.
    Net::LoopbackNetwork network(2);
    const Challenge::Level& level = *Challenge::find_level("mound");
    Peer a(level, network.endpoint(0), 0), b(level, network.endpoint(1), 1);
    const std::uint64_t fake_hash = 0xDEADBEEF;
    const Replay::Command set = SpellSim::set_program_command(0, fake_hash); // блоба с таким хешем никто не отправлял
    for (int i = 0; i < 100; ++i) {
        a.frame([&](const SpellSim::Simulation&, std::vector<Replay::Command>& out) { if (i == 5) out.push_back(set); });
        b.frame([](const SpellSim::Simulation&, std::vector<Replay::Command>&) {});
        network.step();
    }
    CHECK(a.sim.tick_number() < 20); // команда ждёт блоб, которого нет: мир стоит, а не исполняет что попало
    CHECK(a.sim.programs_set() == 0);
}
