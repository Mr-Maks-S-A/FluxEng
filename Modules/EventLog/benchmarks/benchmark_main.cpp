#include <EventLog/Journal.hpp>

#include <Math/Rng.hpp>

#include <benchmark/benchmark.h>

namespace {

std::vector<std::byte> random_bytes(std::size_t n, std::uint64_t seed) {
    Math::Rng rng(seed);
    std::vector<std::byte> v(n);
    for (std::byte& b : v) b = static_cast<std::byte>(rng.next_u32());
    return v;
}

} // namespace

/// Кодирование полосы 8 + 2 по 4 КиБ: сколько мегабайт данных в секунду защищается.
static void BM_EncodeStripe(benchmark::State& state) {
    const EventLog::ErasureCode code(8, 2);
    auto storage = random_bytes(10 * 4096, 1);
    std::vector<std::span<const std::byte>> data;
    std::vector<std::span<std::byte>> parity;
    for (int i = 0; i < 8; ++i) data.emplace_back(storage.data() + static_cast<std::size_t>(i) * 4096, 4096);
    for (int j = 0; j < 2; ++j) parity.emplace_back(storage.data() + static_cast<std::size_t>(8 + j) * 4096, 4096);
    for (auto _ : state) {
        code.encode(data, parity);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * 8 * 4096);
}
BENCHMARK(BM_EncodeStripe);

/// Восстановление двух потерянных блоков данных из полосы 8 + 2.
static void BM_ReconstructTwo(benchmark::State& state) {
    const EventLog::ErasureCode code(8, 2);
    auto storage = random_bytes(10 * 4096, 2);
    std::vector<std::span<std::byte>> blocks;
    for (int i = 0; i < 10; ++i) blocks.emplace_back(storage.data() + static_cast<std::size_t>(i) * 4096, 4096);
    std::array<bool, 10> present{};
    present.fill(true);
    present[1] = present[6] = false;
    for (auto _ : state) benchmark::DoNotOptimize(code.reconstruct(blocks, present));
    state.SetBytesProcessed(state.iterations() * 2 * 4096);
}
BENCHMARK(BM_ReconstructTwo);

/// Запись 100 000 событий по 24 байта (типичное событие шины) в память: пропускная способность журнала.
static void BM_AppendEvents(benchmark::State& state) {
    const auto payload = random_bytes(24, 3);
    for (auto _ : state) {
        EventLog::MemoryStorage storage;
        auto writer = EventLog::Writer::create(storage).value();
        for (int i = 0; i < 100'000; ++i) writer.append(static_cast<std::uint32_t>(i), 1, payload);
        benchmark::DoNotOptimize(writer.flush());
    }
    state.SetItemsProcessed(state.iterations() * 100'000);
}
BENCHMARK(BM_AppendEvents)->Unit(benchmark::kMillisecond);

/// Чтение 100 000 событий с одним повреждённым блоком в каждой 4-й полосе (восстановление по ходу).
static void BM_ReadWithRepair(benchmark::State& state) {
    EventLog::MemoryStorage storage;
    {
        auto writer = EventLog::Writer::create(storage).value();
        const auto payload = random_bytes(24, 4);
        for (int i = 0; i < 100'000; ++i) writer.append(static_cast<std::uint32_t>(i), 1, payload);
    }
    const std::size_t stripe = 10 * (12 + 4096);
    for (std::size_t s = 0; 64 + s * stripe < storage.bytes().size(); s += 4) storage.bytes()[64 + s * stripe + 1000] ^= std::byte{0x55};
    for (auto _ : state) benchmark::DoNotOptimize(EventLog::read_all(storage));
    state.SetItemsProcessed(state.iterations() * 100'000);
}
BENCHMARK(BM_ReadWithRepair)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
