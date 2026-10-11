/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки NetSystem (google-benchmark).
 *
 * Группы:
 * 1. Сериализация: ByteWriter / ByteReader на типичном пакете команд.
 * 2. Имитация сети: отправка + доставка пачки датаграмм.
 * 3. Lockstep: стоимость ОДНОГО тика всех узлов (приём, отправка, шаг) для 2…16 игроков, звезда и mesh, без потерь и с 10% потерь.
 *
 * Шаг симуляции здесь пустой, поэтому цифры — цена сетевого слоя. Для осмысленных цифр собирайте в Release.
 */

#include <NetSystem/NetSystem.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

namespace ns = NetSystem;

struct Cmd {
    std::int16_t dx = 0;
    std::int16_t dy = 0;
    std::uint32_t buttons = 0;
};

// -----------------------------------------------------------------------------
// 1. Сериализация
// -----------------------------------------------------------------------------

void BM_ByteWriter_InputsPacket(benchmark::State& state) {
    const std::vector<Cmd> cmds(24, Cmd{3, -4, 5});
    for (auto _ : state) {
        ns::ByteWriter w;
        w.write<std::uint8_t>(1).write<std::uint16_t>(3).write<std::uint32_t>(1000).write<std::uint16_t>(24);
        for (const Cmd& c : cmds) w.write(c);
        benchmark::DoNotOptimize(w.bytes().data());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ByteWriter_InputsPacket);

void BM_ByteReader_InputsPacket(benchmark::State& state) {
    ns::ByteWriter w;
    w.write<std::uint8_t>(1).write<std::uint16_t>(3).write<std::uint32_t>(1000).write<std::uint16_t>(24);
    for (int i = 0; i < 24; ++i) w.write(Cmd{3, -4, 5});
    const auto bytes = w.take();
    for (auto _ : state) {
        ns::ByteReader r(bytes);
        (void)r.read<std::uint8_t>();
        (void)r.read<std::uint16_t>();
        (void)r.read<std::uint32_t>();
        const auto count = r.read<std::uint16_t>();
        std::uint32_t sum = 0;
        for (std::uint16_t i = 0; i < count; ++i) sum += r.read<Cmd>().buttons;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ByteReader_InputsPacket);

// -----------------------------------------------------------------------------
// 2. Имитация сети
// -----------------------------------------------------------------------------

void BM_SimNetwork_Send1000AndDeliver(benchmark::State& state) {
    ns::SimulatedNetwork net(1, {.latency_us = 30'000, .jitter_us = 10'000, .loss = 0.05});
    ns::ITransport& a = net.add_endpoint();
    ns::ITransport& b = net.add_endpoint();
    const std::vector<std::byte> data(200);
    std::uint64_t now = 0;
    for (auto _ : state) {
        for (int i = 0; i < 1000; ++i) a.send(1, data);
        now += 100'000;
        net.advance_to(now);
        ns::Packet p;
        while (b.receive(p)) benchmark::DoNotOptimize(p.data.data());
    }
    state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_SimNetwork_Send1000AndDeliver);

// -----------------------------------------------------------------------------
// 3. Lockstep
// -----------------------------------------------------------------------------

struct World {
    ns::SimulatedNetwork net;
    std::vector<std::unique_ptr<ns::LockstepNode<Cmd>>> nodes;
    std::uint64_t now = 0;

    World(int players, bool mesh, double loss) : net(1, {.latency_us = 30'000, .jitter_us = 10'000, .loss = loss}) {
        std::vector<ns::ITransport*> endpoints;
        for (int i = 0; i < players; ++i) endpoints.push_back(&net.add_endpoint());
        for (int i = 0; i < players; ++i) {
            ns::LockstepNodeConfig config;
            config.lockstep.self = static_cast<ns::PeerId>(i);
            for (int p = 0; p < players; ++p) config.lockstep.players.push_back(static_cast<ns::PeerId>(p));
            for (int p = 0; p < players; ++p) {
                if (mesh ? p != i : (i == 0 ? p != 0 : p == 0)) config.lockstep.neighbors.push_back(static_cast<ns::PeerId>(p));
            }
            config.lockstep.relay = !mesh && i == 0; // звезда: ретранслирует сервер; mesh: каждый шлёт только своё
            nodes.push_back(std::make_unique<ns::LockstepNode<Cmd>>(
                *endpoints[static_cast<std::size_t>(i)], config, [](ns::PeerId p, std::uint32_t t) { return Cmd{static_cast<std::int16_t>(p), static_cast<std::int16_t>(t), 1}; },
                [](std::span<const Cmd> in, std::uint32_t) { benchmark::DoNotOptimize(in.data()); }, [] { return std::uint64_t{0}; }));
        }
    }

    /// Прокрутить ровно один тик (33,3 мс виртуального времени по 1 мс).
    void one_tick() {
        for (int ms = 0; ms < 33; ++ms) {
            now += 1000;
            net.advance_to(now);
            for (auto& node : nodes) node->update(now);
        }
    }
};

void BM_Lockstep_Tick(benchmark::State& state) {
    World world(static_cast<int>(state.range(0)), state.range(1) != 0, static_cast<double>(state.range(2)) / 100.0);
    for (int warmup = 0; warmup < 30; ++warmup) world.one_tick();
    for (auto _ : state) world.one_tick();
    state.SetItemsProcessed(state.iterations());
    state.counters["bytes/tick/node"] = benchmark::Counter(
        static_cast<double>(world.net.stats().bytes_sent) / static_cast<double>(world.nodes[0]->tick()) / static_cast<double>(state.range(0)));
}
// Аргументы: игроков, mesh (0 — звезда), потери в процентах.
BENCHMARK(BM_Lockstep_Tick)->Args({2, 0, 0})->Args({4, 0, 0})->Args({4, 1, 0})->Args({8, 0, 0})->Args({8, 1, 0})->Args({16, 0, 0})
    ->Args({4, 0, 10})->Args({8, 1, 10});

} // namespace

BENCHMARK_MAIN();
