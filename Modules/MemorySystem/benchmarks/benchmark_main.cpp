/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки MemorySystem (google-benchmark): арена и пул против malloc/new.
 *
 * Группы:
 * 1. Мелкие выделения: Arena::push против malloc и new.
 * 2. «Кадр»: N выделений и освобождение всего разом (reset) против N пар new/delete.
 * 3. Пул: allocate/free против new/delete.
 * 4. pmr::vector в арене против std::vector.
 * 5. Цена инварианта ZII: reset зануляет использованное — сколько это стоит на мегабайт.
 *
 * Для осмысленных цифр собирайте в Release.
 */

#include <MemorySystem/MemorySystem.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <vector>

namespace {

namespace ms = MemorySystem;

struct Particle {
    float x, y, vx, vy, life;
    std::uint32_t flags;
};

// -----------------------------------------------------------------------------
// 1. Мелкие выделения
// -----------------------------------------------------------------------------

void BM_Arena_Push32(benchmark::State& state) {
    ms::Arena arena = ms::Arena::reserve(ms::GiB(1));
    for (auto _ : state) {
        void* p = arena.push(32, 16);
        benchmark::DoNotOptimize(p);
        if (arena.used() > ms::MiB(256)) {
            state.PauseTiming();
            arena.reset();
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Arena_Push32);

void BM_Malloc32(benchmark::State& state) {
    std::vector<void*> blocks;
    blocks.reserve(1 << 20);
    for (auto _ : state) {
        void* p = std::malloc(32);
        benchmark::DoNotOptimize(p);
        blocks.push_back(p);
        if (blocks.size() == blocks.capacity()) {
            state.PauseTiming();
            for (void* b : blocks) std::free(b);
            blocks.clear();
            state.ResumeTiming();
        }
    }
    for (void* b : blocks) std::free(b);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Malloc32);

// -----------------------------------------------------------------------------
// 2. «Кадр»: N объектов, потом освобождение всего
// -----------------------------------------------------------------------------

void BM_Frame_Arena(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    ms::Arena arena = ms::Arena::reserve(ms::MiB(256));
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            Particle* p = arena.push<Particle>();
            p->life = 1.0f;
            benchmark::DoNotOptimize(p);
        }
        arena.reset(); // включая зануление использованного
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Frame_Arena)->Arg(1'000)->Arg(10'000)->Arg(100'000);

void BM_Frame_NewDelete(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::vector<Particle*> items(count);
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            items[i] = new Particle{};
            items[i]->life = 1.0f;
            benchmark::DoNotOptimize(items[i]);
        }
        for (Particle* p : items) delete p;
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Frame_NewDelete)->Arg(1'000)->Arg(10'000)->Arg(100'000);

// -----------------------------------------------------------------------------
// 3. Пул: объекты живут и умирают по одному
// -----------------------------------------------------------------------------

void BM_Pool_AllocFree(benchmark::State& state) {
    auto pool = ms::Pool<Particle>::reserve(1 << 16);
    std::vector<Particle*> live(1024);
    for (auto& p : live) p = pool.allocate();
    std::size_t cursor = 0;
    for (auto _ : state) {
        pool.free(live[cursor]);
        live[cursor] = pool.allocate();
        benchmark::DoNotOptimize(live[cursor]);
        cursor = (cursor + 1) & 1023u;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Pool_AllocFree);

void BM_NewDelete_AllocFree(benchmark::State& state) {
    std::vector<Particle*> live(1024);
    for (auto& p : live) p = new Particle{};
    std::size_t cursor = 0;
    for (auto _ : state) {
        delete live[cursor];
        live[cursor] = new Particle{};
        benchmark::DoNotOptimize(live[cursor]);
        cursor = (cursor + 1) & 1023u;
    }
    for (Particle* p : live) delete p;
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NewDelete_AllocFree);

// -----------------------------------------------------------------------------
// 4. Контейнеры
// -----------------------------------------------------------------------------

void BM_PmrVector_Arena(benchmark::State& state) {
    const auto count = static_cast<int>(state.range(0));
    ms::Arena arena = ms::Arena::reserve(ms::MiB(256));
    for (auto _ : state) {
        {
            ms::ArenaResource resource(arena);
            std::pmr::vector<int> values(&resource);
            for (int i = 0; i < count; ++i) values.push_back(i);
            benchmark::DoNotOptimize(values.data());
        }
        arena.reset();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_PmrVector_Arena)->Arg(1'000)->Arg(100'000);

void BM_StdVector(benchmark::State& state) {
    const auto count = static_cast<int>(state.range(0));
    for (auto _ : state) {
        std::vector<int> values;
        for (int i = 0; i < count; ++i) values.push_back(i);
        benchmark::DoNotOptimize(values.data());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_StdVector)->Arg(1'000)->Arg(100'000);

// -----------------------------------------------------------------------------
// 5. Цена инварианта ZII: reset после использования `range` байт
// -----------------------------------------------------------------------------

void BM_Arena_ResetZeroing(benchmark::State& state) {
    const auto bytes = static_cast<std::size_t>(state.range(0));
    ms::Arena arena = ms::Arena::reserve(ms::MiB(64));
    for (auto _ : state) {
        state.PauseTiming();
        benchmark::DoNotOptimize(arena.push(bytes, 64));
        state.ResumeTiming();
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Arena_ResetZeroing)->Arg(64 << 10)->Arg(1 << 20)->Arg(16 << 20);

} // namespace

BENCHMARK_MAIN();
