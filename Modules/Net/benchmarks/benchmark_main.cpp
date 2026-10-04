#include <Net/Lockstep.hpp>

#include <Math/Rng.hpp>

#include <benchmark/benchmark.h>

#include <memory>

namespace {

Net::Packet inputs_packet(int ticks, int commands_per_tick) {
    Net::Inputs in;
    in.acked = 100;
    for (int t = 0; t < ticks; ++t) {
        Net::TickInputs ti{static_cast<std::uint32_t>(100 + t), {}};
        for (int c = 0; c < commands_per_tick; ++c) ti.commands.push_back({.type = 1, .arg = 0, .x = c, .y = t, .z = 5});
        in.ticks.push_back(std::move(ti));
    }
    return Net::Packet{1, std::move(in)};
}

} // namespace

/// Кодирование пакета ввода: 8 неподтверждённых тиков по 2 команды (типичная нагрузка на задержке ~130 мс).
static void BM_EncodeInputs(benchmark::State& state) {
    const Net::Packet p = inputs_packet(8, 2);
    for (auto _ : state) benchmark::DoNotOptimize(Net::encode(p));
}
BENCHMARK(BM_EncodeInputs);

/// Разбор того же пакета с проверкой контрольной суммы и границ.
static void BM_DecodeInputs(benchmark::State& state) {
    const auto bytes = Net::encode(inputs_packet(8, 2));
    for (auto _ : state) benchmark::DoNotOptimize(Net::decode(bytes));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes.size()));
}
BENCHMARK(BM_DecodeInputs);

/// Пустой тик (ничего не нажато): самый частый пакет — чем он дешевле, тем лучше.
static void BM_DecodeEmptyTicks(benchmark::State& state) {
    const auto bytes = Net::encode(inputs_packet(8, 0));
    for (auto _ : state) benchmark::DoNotOptimize(Net::decode(bytes));
}
BENCHMARK(BM_DecodeEmptyTicks);

/// Кадр сети целиком (pump + submit + advance всех пиров + шаг сети), идеальная сеть: стоимость протокола на тик.
static void BM_LockstepFrame(benchmark::State& state) {
    const auto peers = static_cast<std::size_t>(state.range(0));
    Net::LoopbackNetwork network(peers, {.latency_steps = 1});
    std::vector<std::unique_ptr<Net::Lockstep>> nodes;
    for (std::size_t i = 0; i < peers; ++i) {
        nodes.push_back(std::make_unique<Net::Lockstep>(network.endpoint(static_cast<Net::PeerId>(i)),
                        Net::LockstepConfig{.local = static_cast<Net::PeerId>(i), .peers = static_cast<std::uint8_t>(peers)}));
    }
    const Replay::Command cmd{.type = 1, .x = 1};
    std::uint64_t steps = 0;
    for (auto _ : state) {
        for (auto& n : nodes) {
            n->pump();
            if (n->needs_input()) n->submit(std::span(&cmd, 1));
            if (n->ready()) { benchmark::DoNotOptimize(n->advance()); ++steps; }
        }
        network.step();
    }
    state.counters["ticks/peer"] = benchmark::Counter(static_cast<double>(steps) / static_cast<double>(peers), benchmark::Counter::kAvgThreads);
}
BENCHMARK(BM_LockstepFrame)->Arg(2)->Arg(4)->Arg(8);

/// То же на плохой сети (потери 15 %, разброс): цена повторных отправок.
static void BM_LockstepFrameLossy(benchmark::State& state) {
    Net::LoopbackNetwork network(2, {.latency_steps = 2, .jitter_steps = 3, .loss_permille = 150}, 3);
    Net::Lockstep a(network.endpoint(0), {.local = 0, .peers = 2}), b(network.endpoint(1), {.local = 1, .peers = 2});
    const Replay::Command cmd{.type = 1, .x = 1};
    for (auto _ : state) {
        for (Net::Lockstep* n : {&a, &b}) {
            n->pump();
            if (n->needs_input()) n->submit(std::span(&cmd, 1));
            if (n->ready()) benchmark::DoNotOptimize(n->advance());
        }
        network.step();
    }
    state.counters["ticks"] = static_cast<double>(a.tick());
    state.counters["KB sent"] = static_cast<double>(network.stats().bytes) / 1024.0;
}
BENCHMARK(BM_LockstepFrameLossy);

/// Доставка блоба 1,3 КиБ (типичная программа заклинания) по сети с потерями 15 %: сколько шагов сети до подтверждения.
static void BM_BlobDelivery(benchmark::State& state) {
    std::vector<std::byte> program(1300);
    Math::Rng rng(1);
    for (auto& b : program) b = static_cast<std::byte>(rng.next_u32());
    std::uint64_t total_steps = 0;
    for (auto _ : state) {
        Net::LoopbackNetwork network(2, {.latency_steps = 2, .loss_permille = 150}, total_steps + 1);
        Net::Lockstep a(network.endpoint(0), {.local = 0, .peers = 2}), b(network.endpoint(1), {.local = 1, .peers = 2});
        const std::uint64_t hash = a.submit_blob(program);
        int steps = 0;
        while (!b.blob(hash) && steps < 1000) {
            a.pump(); b.pump(); network.step();
            ++steps;
        }
        total_steps += static_cast<std::uint64_t>(steps);
    }
    state.counters["steps/blob"] = benchmark::Counter(static_cast<double>(total_steps), benchmark::Counter::kAvgIterations);
}
BENCHMARK(BM_BlobDelivery);

BENCHMARK_MAIN();
