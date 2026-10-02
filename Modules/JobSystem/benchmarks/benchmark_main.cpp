/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки JobSystem (google-benchmark).
 *
 * Группы:
 * 1. Накладные расходы: run+wait одной пустой задачи, пачки задач, пустой parallel_for.
 * 2. Масштабирование: «лёгкий» (движение) и «тяжёлый» (поиск соседей) цикл при 0…N потоках.
 * 3. Влияние grain: один и тот же цикл, куски от 64 до 65536 элементов.
 * 4. Детерминированный сбор событий: ChunkBuffers + слияние против последовательного push_back.
 *
 * Аргумент бенчмарков группы 2 — число фоновых потоков. Для осмысленных цифр собирайте в Release.
 */

#include <JobSystem/JobSystem.hpp>

#include <benchmark/benchmark.h>

#include <cmath>
#include <atomic>
#include <cstdint>
#include <vector>

namespace {

namespace js = JobSystem;

// -----------------------------------------------------------------------------
// 1. Накладные расходы
// -----------------------------------------------------------------------------

void BM_RunWait_Single(benchmark::State& state) {
    js::Scheduler jobs({.threads = static_cast<unsigned>(state.range(0))});
    for (auto _ : state) {
        js::JobCounter counter;
        jobs.run(counter, [] {});
        jobs.wait(counter);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RunWait_Single)->Arg(0)->Arg(1)->Arg(3)->UseRealTime();

void BM_RunWait_Batch1000(benchmark::State& state) {
    js::Scheduler jobs({.threads = static_cast<unsigned>(state.range(0))});
    std::atomic<int> sink{0};
    for (auto _ : state) {
        js::JobCounter counter;
        for (int i = 0; i < 1000; ++i) jobs.run(counter, [&sink] { sink.fetch_add(1, std::memory_order_relaxed); });
        jobs.wait(counter);
    }
    state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_RunWait_Batch1000)->Arg(0)->Arg(1)->Arg(3)->UseRealTime();

void BM_ParallelFor_Empty(benchmark::State& state) {
    js::Scheduler jobs({.threads = static_cast<unsigned>(state.range(0))});
    for (auto _ : state) {
        js::parallel_for(jobs, 64, 1, [](std::size_t, std::size_t) {});
    }
    state.SetItemsProcessed(state.iterations() * 64); // кусков в секунду: цена раздачи пустой работы
}
BENCHMARK(BM_ParallelFor_Empty)->Arg(0)->Arg(1)->Arg(3)->UseRealTime();

// -----------------------------------------------------------------------------
// 2. Масштабирование
// -----------------------------------------------------------------------------

struct Bodies {
    std::vector<float> x, y, vx, vy;
    explicit Bodies(std::size_t n) : x(n), y(n), vx(n), vy(n) {
        for (std::size_t i = 0; i < n; ++i) {
            x[i] = static_cast<float>(i % 1000);
            y[i] = static_cast<float>(i / 1000);
            vx[i] = std::cos(static_cast<float>(i));
            vy[i] = std::sin(static_cast<float>(i));
        }
    }
};

// Лёгкая работа: ~1 нс на элемент, упирается в память.
void BM_Integrate_1M(benchmark::State& state) {
    js::Scheduler jobs({.threads = static_cast<unsigned>(state.range(0))});
    Bodies b(1'000'000);
    for (auto _ : state) {
        js::parallel_for(jobs, b.x.size(), 16384, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                b.x[i] += b.vx[i] * 0.016f;
                b.y[i] += b.vy[i] * 0.016f;
            }
        });
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(b.x.size()));
}
BENCHMARK(BM_Integrate_1M)->DenseRange(0, 3)->UseRealTime()->Unit(benchmark::kMicrosecond);

// Тяжёлая работа: каждый элемент просматривает 64 соседа (как steering в Swarm), упирается в вычисления.
void BM_Neighbours_100k(benchmark::State& state) {
    js::Scheduler jobs({.threads = static_cast<unsigned>(state.range(0))});
    Bodies b(100'000);
    std::vector<float> ax(b.x.size()), ay(b.x.size());
    for (auto _ : state) {
        js::parallel_for(jobs, b.x.size(), 512, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                float fx = 0.0f, fy = 0.0f;
                for (std::size_t k = 1; k <= 64; ++k) {
                    const std::size_t j = (i + k * 97) % b.x.size();
                    const float dx = b.x[j] - b.x[i], dy = b.y[j] - b.y[i];
                    const float inv = 1.0f / (dx * dx + dy * dy + 1.0f);
                    fx += dx * inv;
                    fy += dy * inv;
                }
                ax[i] = fx;
                ay[i] = fy;
            }
        });
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(b.x.size()));
}
BENCHMARK(BM_Neighbours_100k)->DenseRange(0, 3)->UseRealTime()->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// 3. Влияние grain
// -----------------------------------------------------------------------------

void BM_Grain(benchmark::State& state) {
    js::Scheduler jobs({.threads = js::default_threads()});
    Bodies b(1'000'000);
    const auto grain = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        js::parallel_for(jobs, b.x.size(), grain, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) b.x[i] += std::sqrt(b.vx[i] * b.vx[i] + b.vy[i] * b.vy[i]);
        });
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(b.x.size()));
}
BENCHMARK(BM_Grain)->RangeMultiplier(4)->Range(64, 65536)->UseRealTime()->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// 4. Детерминированный сбор событий
// -----------------------------------------------------------------------------

struct Event {
    std::uint32_t who;
    float value;
};

void BM_Events_Serial(benchmark::State& state) {
    Bodies b(1'000'000);
    std::vector<Event> out;
    for (auto _ : state) {
        out.clear();
        for (std::size_t i = 0; i < b.x.size(); ++i) {
            const float s = std::sqrt(b.vx[i] * b.vx[i] + b.vy[i] * b.vy[i]) * std::sin(b.x[i]);
            if (s > 0.9f) out.push_back({static_cast<std::uint32_t>(i), s});
        }
        benchmark::DoNotOptimize(out.data());
    }
    state.counters["events"] = static_cast<double>(out.size());
}
BENCHMARK(BM_Events_Serial)->UseRealTime()->Unit(benchmark::kMicrosecond);

void BM_Events_ChunkBuffers(benchmark::State& state) {
    js::Scheduler jobs({.threads = static_cast<unsigned>(state.range(0))});
    Bodies b(1'000'000);
    js::ChunkBuffers<Event> chunks;
    std::vector<Event> out;
    constexpr std::size_t grain = 16384;
    for (auto _ : state) {
        chunks.reset(js::chunk_count(b.x.size(), grain));
        js::parallel_for(jobs, b.x.size(), grain, [&](std::size_t begin, std::size_t end, std::size_t chunk) {
            for (std::size_t i = begin; i < end; ++i) {
                const float s = std::sqrt(b.vx[i] * b.vx[i] + b.vy[i] * b.vy[i]) * std::sin(b.x[i]);
                if (s > 0.9f) chunks[chunk].push_back({static_cast<std::uint32_t>(i), s});
            }
        });
        out.clear();
        chunks.for_each([&](const Event& e) { out.push_back(e); }); // слияние в порядке кусков
        benchmark::DoNotOptimize(out.data());
    }
    state.counters["events"] = static_cast<double>(out.size());
}
BENCHMARK(BM_Events_ChunkBuffers)->DenseRange(0, 3)->UseRealTime()->Unit(benchmark::kMicrosecond);

} // namespace

BENCHMARK_MAIN();
