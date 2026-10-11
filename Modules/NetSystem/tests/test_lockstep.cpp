#include <NetSystem/NetSystem.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

using namespace NetSystem;

namespace {

struct Cmd {
    std::int16_t dx = 0;
    std::int16_t dy = 0;
    std::uint32_t buttons = 0;
};
static_assert(std::has_unique_object_representations_v<Cmd>);

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 1099511628211ull + 0x9E3779B97F4A7C15ull; }

/// Ввод игрока на тик: чистая функция от (игрок, тик) — эталон считается независимо от сети.
Cmd sample(PeerId peer, std::uint32_t tick) {
    const std::uint64_t h = mix(mix(1469598103934665603ull, peer), tick);
    return Cmd{static_cast<std::int16_t>(static_cast<int>(h & 0x1F) - 16), static_cast<std::int16_t>(static_cast<int>((h >> 8) & 0x1F) - 16), static_cast<std::uint32_t>((h >> 16) & 0xF)};
}

/// «Симуляция»: хеш-цепочка по командам и номеру тика; история хеша после каждого тика.
struct Sim {
    std::uint64_t hash = 0x1234;
    std::vector<std::uint64_t> history{0x1234};
    void step(std::span<const Cmd> inputs, std::uint32_t tick) {
        for (const Cmd& c : inputs) hash = mix(mix(mix(hash, static_cast<std::uint16_t>(c.dx)), static_cast<std::uint16_t>(c.dy)), c.buttons);
        hash = mix(hash, tick);
        history.push_back(hash);
    }
};

/// Эталон без сети: тик t < input_delay идёт с пустыми командами, дальше — sample().
std::vector<std::uint64_t> reference(int players, int ticks, std::uint32_t input_delay) {
    Sim sim;
    for (int t = 0; t < ticks; ++t) {
        std::vector<Cmd> inputs(static_cast<std::size_t>(players));
        for (int p = 0; p < players; ++p) {
            if (static_cast<std::uint32_t>(t) >= input_delay) inputs[static_cast<std::size_t>(p)] = sample(static_cast<PeerId>(p), static_cast<std::uint32_t>(t));
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
    std::vector<std::unique_ptr<LockstepNode<Cmd>>> nodes;
    Setup setup;

    explicit World(const Setup& s) : net(s.seed, s.link), sims(static_cast<std::size_t>(s.players)), setup(s) {
        std::vector<ITransport*> endpoints;
        for (int i = 0; i < s.players; ++i) endpoints.push_back(&net.add_endpoint());
        for (int i = 0; i < s.players; ++i) {
            LockstepNodeConfig config;
            config.lockstep.self = static_cast<PeerId>(i);
            config.lockstep.input_delay = s.input_delay;
            for (int p = 0; p < s.players; ++p) config.lockstep.players.push_back(static_cast<PeerId>(p));
            for (int p = 0; p < s.players; ++p) {
                const bool linked = s.topology == Topology::Mesh ? p != i : (i == 0 ? p != 0 : p == 0);
                if (linked) config.lockstep.neighbors.push_back(static_cast<PeerId>(p));
            }
            config.lockstep.relay = s.topology == Topology::Star ? i == 0 : s.mesh_relay;
            if (!s.clock_scale.empty()) config.clock_scale = s.clock_scale[static_cast<std::size_t>(i)];
            Sim* sim = &sims[static_cast<std::size_t>(i)];
            const bool corrupt = i == s.corrupt_peer;
            const std::uint32_t corrupt_tick = s.corrupt_tick;
            nodes.push_back(std::make_unique<LockstepNode<Cmd>>(
                *endpoints[static_cast<std::size_t>(i)], config, [](PeerId self, std::uint32_t tick) { return sample(self, tick); },
                [sim, corrupt, corrupt_tick](std::span<const Cmd> inputs, std::uint32_t tick) {
                    sim->step(inputs, tick);
                    if (corrupt && tick == corrupt_tick) {
                        sim->hash ^= 1; // «ошибка в симуляции»: состояние расходится и дальше живёт своей жизнью
                        sim->history.back() = sim->hash;
                    }
                },
                [sim] { return sim->hash; }));
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
    std::uint64_t total_desyncs() const {
        std::uint64_t sum = 0;
        for (const auto& n : nodes) sum += n->session().stats().desyncs;
        return sum;
    }
};

} // namespace

TEST_SUITE("NetSystem.Lockstep") {

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
                World world({.topology = topology, .link = {.latency_us = 45'000, .jitter_us = 40'000, .loss = loss, .duplicate = 0.1}, .seed = seed});
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
        // тик s + input_delay, то есть дойти до sim_tick = s + input_delay + 1.
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
        REQUIRE(culprit.stats().desyncs > 0);
        CHECK(culprit.desyncs().front().tick == 60); // первая контрольная точка после порчи (интервал 30)
        CHECK(world.nodes[0]->session().stats().desyncs > 0);
        CHECK(world.nodes[0]->session().desyncs().front().peer == 2);
        if (topology == Topology::Star) {
            CHECK(world.nodes[1]->session().stats().desyncs == 0); // клиенты видят только сервер, а у них с ним всё сходится
            CHECK(world.nodes[3]->session().stats().desyncs == 0);
        } else {
            CHECK(world.nodes[1]->session().stats().desyncs > 0);
            CHECK(world.nodes[3]->session().stats().desyncs > 0);
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

TEST_CASE("мусор и чужие пакеты: узел их считает и игнорирует, симуляция не страдает") {
    SimulatedNetwork net(1, {.latency_us = 1000});
    ITransport& a = net.add_endpoint();
    ITransport& b = net.add_endpoint();
    ITransport& stranger = net.add_endpoint();
    Sim sim;
    LockstepNodeConfig config;
    config.lockstep.self = 0;
    config.lockstep.players = {0, 1};
    config.lockstep.neighbors = {1};
    LockstepNode<Cmd> node(a, config, [](PeerId p, std::uint32_t t) { return sample(p, t); },
                           [&](std::span<const Cmd> in, std::uint32_t t) { sim.step(in, t); }, [&] { return sim.hash; });

    stranger.send(0, std::vector<std::byte>{std::byte{1}, std::byte{2}}); // не сосед
    std::mt19937 rng(5);
    for (int i = 0; i < 3000; ++i) { // случайные байты от настоящего соседа
        std::vector<std::byte> garbage(rng() % 64);
        for (std::byte& byte : garbage) byte = static_cast<std::byte>(rng());
        if (!garbage.empty() && i % 2 == 0) garbage[0] = std::byte{static_cast<unsigned char>(1 + rng() % 2)}; // с корректным типом
        b.send(0, garbage);
    }
    net.advance_to(2000);
    node.update(2000);
    CHECK(node.session().stats().bad_packets > 1000);
    CHECK(node.session().stats().packets_received >= 3000);
}

TEST_CASE("неверная конфигурация") {
    SimulatedNetwork net;
    ITransport& t = net.add_endpoint();
    LockstepConfig no_self;
    no_self.self = 0;
    no_self.players = {1, 2};
    CHECK_THROWS_AS((Lockstep<Cmd>(t, no_self)), std::invalid_argument);
    LockstepConfig no_window;
    no_window.players = {0};
    no_window.max_window = 0;
    CHECK_THROWS_AS((Lockstep<Cmd>(t, no_window)), std::invalid_argument);
}

} // TEST_SUITE
