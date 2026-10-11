#include <Net/Net.hpp>

#include <Math/Rng.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

using namespace Net;

namespace {

using Command = Replay::Command;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 1099511628211ull + 0x9E3779B97F4A7C15ull; }

/// Ввод игрока на тик: чистая функция от (игрок, тик) — эталон считается независимо от сети.
Command sample(PeerId peer, std::uint32_t tick) {
    const std::uint64_t h = mix(mix(1469598103934665603ull, peer), tick);
    return Command{.type = 1, .arg = 0, .x = static_cast<std::int32_t>(h & 0x1F) - 16, .y = static_cast<std::int32_t>((h >> 8) & 0x1F) - 16,
                   .z = static_cast<std::int32_t>((h >> 16) & 0xF)};
}

/// «Симуляция»: хеш-цепочка по командам и номеру тика; история хеша после каждого тика.
struct Sim {
    std::uint64_t hash = 0x1234;
    std::vector<std::uint64_t> history{0x1234};
    void step(std::span<const Command> inputs, std::uint32_t tick) {
        for (const Command& c : inputs) hash = mix(mix(mix(hash, static_cast<std::uint32_t>(c.x)), static_cast<std::uint32_t>(c.y)), static_cast<std::uint32_t>(c.z));
        hash = mix(hash, tick);
        history.push_back(hash);
    }
};

/// Эталон без сети: тики до input_delay идут без команд, дальше — sample() каждого игрока.
std::vector<std::uint64_t> reference(int players, int ticks, std::uint32_t input_delay) {
    Sim sim;
    for (int t = 0; t < ticks; ++t) {
        std::vector<Command> inputs;
        if (static_cast<std::uint32_t>(t) >= input_delay) {
            for (int p = 0; p < players; ++p) inputs.push_back(sample(static_cast<PeerId>(p), static_cast<std::uint32_t>(t)));
        }
        sim.step(inputs, static_cast<std::uint32_t>(t));
    }
    return sim.history;
}

enum class Topology { Star, Mesh };

struct Setup {
    Topology topology = Topology::Star;
    int players = 4;
    LinkConfig link{.latency_us = 30'000};
    std::uint64_t seed = 1;
    std::uint32_t input_delay = 4;
    std::vector<double> clock_scale{};   // по узлам; пусто — все 1.0
    int corrupt_peer = -1;               // узел, у которого на corrupt_tick портится состояние
    std::uint32_t corrupt_tick = 0;
    bool mesh_relay = false;             // mesh: пересылать ли чужие команды (по умолчанию нет — все и так связаны напрямую)
};

struct World {
    SimulatedNetwork net;
    std::vector<Sim> sims;
    std::vector<std::unique_ptr<Node>> nodes;
    Setup setup;

    explicit World(const Setup& s) : net(s.seed, s.link), sims(static_cast<std::size_t>(s.players)), setup(s) {
        for (int i = 0; i < s.players; ++i) net.add_endpoint();
        for (int i = 0; i < s.players; ++i) {
            NodeConfig config;
            config.lockstep.local = static_cast<PeerId>(i);
            config.lockstep.peers = static_cast<std::uint8_t>(s.players);
            config.lockstep.input_delay = s.input_delay;
            config.lockstep.config_hash = 42;
            if (s.topology == Topology::Star) {
                if (i != 0) config.lockstep.neighbors = {0}; // клиент знает только сервер; у сервера пусто = все
            }
            config.lockstep.relay = s.topology == Topology::Star ? i == 0 : s.mesh_relay;
            if (!s.clock_scale.empty()) config.clock_scale = s.clock_scale[static_cast<std::size_t>(i)];
            Sim* sim = &sims[static_cast<std::size_t>(i)];
            const bool corrupt = i == s.corrupt_peer;
            const std::uint32_t corrupt_tick = s.corrupt_tick;
            nodes.push_back(std::make_unique<Node>(
                net.endpoint(static_cast<PeerId>(i)), config,
                [](PeerId self, std::uint32_t tick, std::vector<Command>& out) { out.push_back(sample(self, tick)); },
                [sim, corrupt, corrupt_tick](std::span<const Command> inputs, std::uint32_t tick) {
                    sim->step(inputs, tick);
                    if (corrupt && tick == corrupt_tick) {
                        sim->hash ^= 1; // «ошибка в симуляции»: состояние расходится и дальше живёт своей жизнью
                        sim->history.back() = sim->hash;
                    }
                },
                [sim] {
                    Replay::StateHashes h;
                    h.add("sim", sim->hash);
                    return h;
                }));
        }
    }

    /// Крутит виртуальное время по 1 мс, пока все узлы не дойдут до `ticks` или не истечёт `timeout_us`.
    bool run(std::uint32_t ticks, std::uint64_t timeout_us = 120'000'000) {
        for (std::uint64_t now = 0; now <= timeout_us; now += 1000) {
            net.advance_to(now);
            for (auto& node : nodes) node->update(now);
            if (std::ranges::all_of(nodes, [&](const auto& n) { return n->tick() >= ticks; })) return true;
        }
        return false;
    }

    /// Хеши всех узлов совпадают с эталоном на каждом тике, который узел успел выполнить (до `ticks`).
    bool matches_reference(std::uint32_t ticks) const {
        const auto expected = reference(setup.players, static_cast<int>(ticks), setup.input_delay);
        for (const Sim& sim : sims) {
            const std::size_t n = std::min<std::size_t>(sim.history.size(), expected.size());
            if (!std::equal(sim.history.begin(), sim.history.begin() + static_cast<std::ptrdiff_t>(n), expected.begin())) return false;
        }
        return true;
    }

    std::uint64_t total_input_conflicts() const {
        std::uint64_t sum = 0;
        for (const auto& n : nodes) sum += n->session().stats().input_conflicts;
        return sum;
    }
    int total_desyncs() const {
        int sum = 0;
        for (const auto& n : nodes) sum += n->session().desync().has_value() ? 1 : 0;
        return sum;
    }
};

} // namespace

TEST_SUITE("Net.Node") {

TEST_CASE("звезда и mesh: хорошая сеть — все узлы совпадают с эталоном на каждом тике, простоев нет") {
    for (const Topology topology : {Topology::Star, Topology::Mesh}) {
        CAPTURE(static_cast<int>(topology));
        World world({.topology = topology});
        REQUIRE(world.run(240));
        CHECK(world.matches_reference(240));
        CHECK(world.total_input_conflicts() == 0);
        CHECK(world.total_desyncs() == 0);
        for (const auto& node : world.nodes) CHECK(node->stalls() == 0); // задержка сети меньше задержки ввода
    }
}

TEST_CASE("плохая сеть: потери 10–30%, джиттер, дубликаты — состояние всё равно одинаково (много seed'ов)") {
    for (const Topology topology : {Topology::Star, Topology::Mesh}) {
        for (const double loss : {0.1, 0.3}) {
            for (std::uint64_t seed = 1; seed <= 6; ++seed) {
                CAPTURE(static_cast<int>(topology));
                CAPTURE(loss);
                CAPTURE(seed);
                World world({.topology = topology, .link = {.latency_us = 45'000, .jitter_us = 40'000, .loss = loss, .duplicate = 0.1, .corrupt = 0.02}, .seed = seed});
                REQUIRE(world.run(180));
                CHECK(world.matches_reference(180));
                CHECK(world.total_input_conflicts() == 0);
                CHECK(world.total_desyncs() == 0);
            }
        }
    }
}

TEST_CASE("задержка сети больше задержки ввода: узлы простаивают, но состояние не расходится") {
    World world({.link = {.latency_us = 150'000}}); // 300 мс туда-обратно при 133 мс запаса
    REQUIRE(world.run(120));
    CHECK(world.matches_reference(120));
    std::uint64_t stalls = 0;
    for (const auto& node : world.nodes) stalls += node->stalls();
    CHECK(stalls > 0);
}

TEST_CASE("больше input_delay — меньше простоев") {
    const auto stalls_with = [](std::uint32_t delay) {
        World world({.link = {.latency_us = 80'000}, .input_delay = delay});
        REQUIRE(world.run(150));
        std::uint64_t stalls = 0;
        for (const auto& node : world.nodes) stalls += node->stalls();
        return stalls;
    };
    CHECK(stalls_with(2) > stalls_with(8));
    CHECK(stalls_with(8) == 0);
}

TEST_CASE("часы узлов идут с разной скоростью: быстрый ждёт медленного, отрыв не больше input_delay + 1, результат тот же") {
    World world({.topology = Topology::Mesh, .clock_scale = {1.0, 0.8, 1.3, 1.1}});
    for (std::uint64_t now = 0; now < 8'000'000; now += 1000) {
        world.net.advance_to(now);
        for (auto& node : world.nodes) node->update(now);
        std::uint32_t lo = ~0u, hi = 0;
        for (const auto& node : world.nodes) {
            lo = std::min(lo, node->tick());
            hi = std::max(hi, node->tick());
        }
        // Медленный узел на тике s уже объявил команды до тика s + input_delay включительно; быстрый может выполнить
        // тик s + input_delay, то есть дойти до tick = s + input_delay + 1.
        REQUIRE(hi - lo <= 4 + 1);
    }
    CHECK(world.nodes[0]->tick() > 100);
    CHECK(world.matches_reference(world.nodes[0]->tick()));
}

TEST_CASE("рассинхронизация ловится: звезда — сервер и виновник, mesh — все, кто сравнивает с виновником") {
    for (const Topology topology : {Topology::Star, Topology::Mesh}) {
        CAPTURE(static_cast<int>(topology));
        World world({.topology = topology, .link = {.latency_us = 20'000, .loss = 0.05}, .corrupt_peer = 2, .corrupt_tick = 50});
        REQUIRE(world.run(200));
        CHECK_FALSE(world.matches_reference(200));

        const auto& culprit = world.nodes[2]->session();
        REQUIRE(culprit.desync().has_value());
        CHECK(culprit.desync()->tick == 59); // первая контрольная точка после порчи: хеши после 60 исполненных тиков (интервал 30)
        CHECK(culprit.desync()->subsystems == std::vector<std::string>{"sim"});
        REQUIRE(world.nodes[0]->session().desync().has_value());
        CHECK(world.nodes[0]->session().desync()->peer == 2);
        if (topology == Topology::Star) {
            CHECK_FALSE(world.nodes[1]->session().desync().has_value()); // клиенты видят только сервер, а у них с ним всё сходится
            CHECK_FALSE(world.nodes[3]->session().desync().has_value());
        } else {
            CHECK(world.nodes[1]->session().desync().has_value());
            CHECK(world.nodes[3]->session().desync().has_value());
        }
    }
}

TEST_CASE("mesh без пересылки чужих команд шлёт заметно меньше, а результат тот же (и с пересылкой — тоже)") {
    std::uint64_t bytes[2] = {};
    for (const bool relay : {false, true}) {
        World world({.topology = Topology::Mesh, .players = 6, .link = {.latency_us = 30'000, .jitter_us = 15'000, .loss = 0.1}, .mesh_relay = relay});
        REQUIRE(world.run(150));
        CHECK(world.matches_reference(150));
        CHECK(world.total_desyncs() == 0);
        bytes[relay ? 1 : 0] = world.net.stats().bytes_sent;
    }
    CHECK(bytes[0] * 3 < bytes[1] * 2); // экономия не менее чем в 1,5 раза (остальное — подтверждения, они нужны в любом случае)
}

TEST_CASE("один игрок без соседей работает сам") {
    World world({.players = 1});
    REQUIRE(world.run(100));
    CHECK(world.matches_reference(100));
}

TEST_CASE("восемь игроков, mesh: пакеты укладываются в MTU, состояние совпадает") {
    World world({.topology = Topology::Mesh, .players = 8, .link = {.latency_us = 25'000, .jitter_us = 10'000, .loss = 0.05}});
    REQUIRE(world.run(150));
    CHECK(world.matches_reference(150));
    CHECK(world.net.stats().oversized == 0);
}

TEST_CASE("шестнадцать игроков, звезда: сервер пересылает всех, пакеты укладываются в MTU") {
    World world({.topology = Topology::Star, .players = 16, .link = {.latency_us = 25'000, .jitter_us = 10'000, .loss = 0.05}});
    REQUIRE(world.run(120));
    CHECK(world.matches_reference(120));
    CHECK(world.net.stats().oversized == 0);
}

TEST_CASE("блоб через сервер звезды: клиент отправляет, другой клиент получает и исполнение ждёт его") {
    World world({.topology = Topology::Star, .players = 3, .link = {.latency_us = 20'000, .loss = 0.15}, .seed = 5});
    std::vector<std::byte> program(2500);
    for (std::size_t i = 0; i < program.size(); ++i) program[i] = static_cast<std::byte>(i * 7);
    std::uint64_t hash = 0;
    for (auto& node : world.nodes) {
        node->session().set_blob_reference([&hash](const Command& c) -> std::optional<std::uint64_t> {
            if (c.type != 9) return std::nullopt;
            return hash;
        });
    }
    hash = world.nodes[1]->session().submit_blob(program);
    REQUIRE(world.run(100));
    for (const auto& node : world.nodes) {
        REQUIRE(node->session().blob(hash) != nullptr);
        CHECK(*node->session().blob(hash) == program);
    }
    CHECK(world.nodes[2]->session().stats().blobs_received == 1);
    CHECK(world.nodes[0]->session().stats().blobs_received == 1);
}

TEST_CASE("мусор и чужие пакеты: узел их считает и игнорирует, симуляция не страдает") {
    SimulatedNetwork net(1, {.latency_us = 1000});
    Transport& a = net.add_endpoint();
    Transport& b = net.add_endpoint();
    Transport& stranger = net.add_endpoint();
    Sim sim;
    NodeConfig config;
    config.lockstep.local = 0;
    config.lockstep.peers = 2;
    config.lockstep.neighbors = {1};
    Node node(a, config, [](PeerId p, std::uint32_t t, std::vector<Command>& out) { out.push_back(sample(p, t)); },
              [&](std::span<const Command> in, std::uint32_t t) { sim.step(in, t); }, [&] { return Replay::StateHashes{}; });

    stranger.send(0, std::vector<std::byte>{std::byte{1}, std::byte{2}}); // не сосед
    Math::Rng rng(5);
    for (int i = 0; i < 3000; ++i) { // случайные байты от настоящего соседа
        std::vector<std::byte> garbage(rng.below(64));
        for (std::byte& byte : garbage) byte = static_cast<std::byte>(rng.next_u32());
        b.send(0, garbage);
    }
    net.advance_to(2000);
    node.update(2000);
    CHECK(node.session().stats().bad_packets >= 3000);
    CHECK(node.session().stats().packets_received == 0);
}

} // TEST_SUITE
