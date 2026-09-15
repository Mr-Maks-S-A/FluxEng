#include <benchmark/benchmark.h>

#include <EventSystem/Bus/EventBus.hpp>
#include <EventSystem/Core/DeterministicTypeRegistry.hpp>
#include <EventSystem/Core/FNV1a.hpp>
#include <EventSystem/Storage/SoAEventBucket.hpp>

#include <cstdint>
#include <numeric>
#include <random>
#include <string_view>
#include <vector>

// =============================================================================
// 0. РЕГИСТРАЦИЯ ТИПОВ ДЛЯ БЕНЧМАРКА
// =============================================================================

using BenchmarkSmallSoA = EventSystem::Storage::SoAEventBucket<uint32_t, float>;
using BenchmarkPhysicsSoA = EventSystem::Storage::SoAEventBucket<uint64_t, double, double, double>;

struct PhysicsEventAoS {
    uint64_t entity_id;
    double x, y, z;
};

REGISTER_EVENT_TYPE(BenchmarkSmallSoA)
REGISTER_EVENT_TYPE(BenchmarkPhysicsSoA)

// =============================================================================
// 1. БЕНЧМАРК ХЭШИРОВАНИЯ И РЕГИСТРАЦИИ (Compile-Time)
// =============================================================================

static void BM_Core_FNV1a_CompileTime(benchmark::State& state) {
    for (auto _ : state) {
        // Локальная не-const переменная, чтобы удовлетворить DoNotOptimize без deprecation warning
        auto hash = EventSystem::Core::FNV1a::hash("BenchmarkPhysicsSoA");
        benchmark::DoNotOptimize(hash);
    }
}
BENCHMARK(BM_Core_FNV1a_CompileTime);

static void BM_Core_DeterministicTypeRegistry_GetId(benchmark::State& state) {
    for (auto _ : state) {
        auto id = EventSystem::Core::DeterministicTypeRegistry::get_id<BenchmarkPhysicsSoA>();
        benchmark::DoNotOptimize(id);
    }
}
BENCHMARK(BM_Core_DeterministicTypeRegistry_GetId);

// =============================================================================
// 2. БЕНЧМАРКИ ЗАПИСИ (EMIT / PUSH) И ПАМЯТИ
// =============================================================================

static void BM_SoAEventBucket_Push_Direct(benchmark::State& state) {
    BenchmarkPhysicsSoA bucket;
    bucket.reserve(state.range(0));

    uint64_t id = 0;
    for (auto _ : state) {
        bucket.push(id++, 1.0, 2.0, 3.0);
        if (bucket.size() >= static_cast<size_t>(state.range(0))) {
            state.PauseTiming();
            bucket.clear();
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["AllocatedBytes"] = benchmark::Counter(
        static_cast<double>(bucket.allocated_bytes()), benchmark::Counter::kDefaults);
}
BENCHMARK(BM_SoAEventBucket_Push_Direct)->Range(1024, 1 << 18);

static void BM_EventBus_Emit_Reserved(benchmark::State& state) {
    EventSystem::Bus::EventBus bus;
    const size_t reserve_cap = state.range(0);
    bus.register_event<BenchmarkPhysicsSoA>(reserve_cap);

    uint64_t id = 0;
    for (auto _ : state) {
        bus.emit<BenchmarkPhysicsSoA>(id++, 10.0, 20.0, 30.0);

        auto* bucket = bus.get_bucket<BenchmarkPhysicsSoA>();
        if (bucket && bucket->size() >= reserve_cap) {
            state.PauseTiming();
            bucket->clear();
            state.ResumeTiming();
        }
    }

    auto* bucket = bus.get_bucket<BenchmarkPhysicsSoA>();
    state.SetItemsProcessed(state.iterations());
    state.counters["AllocatedBytes"] = benchmark::Counter(
        static_cast<double>(bucket ? bucket->allocated_bytes() : 0), benchmark::Counter::kDefaults);
}
BENCHMARK(BM_EventBus_Emit_Reserved)->Range(1024, 1 << 18);

static void BM_EventBus_Emit_Unreserved(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        EventSystem::Bus::EventBus bus;
        bus.register_event<BenchmarkPhysicsSoA>(0);
        state.ResumeTiming();

        for (int i = 0; i < state.range(0); ++i) {
            bus.emit<BenchmarkPhysicsSoA>(static_cast<uint64_t>(i), 1.0, 2.0, 3.0);
        }
        
        benchmark::DoNotOptimize(bus);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_EventBus_Emit_Unreserved)->Range(1000, 100000);

// =============================================================================
// 3. БЕНЧМАРК ЧТЕНИЯ И ИТЕРАЦИИ: SoA vs AoS (Кэш-эффективность)
// =============================================================================

static void BM_Read_SingleField_SoA(benchmark::State& state) {
    const size_t count = state.range(0);
    BenchmarkPhysicsSoA bucket;
    bucket.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        bucket.push(i, 1.5, 2.5, 3.5);
    }

    for (auto _ : state) {
        double sum = 0.0;
        auto x_stream = bucket.get_stream<1>();
        for (double x : x_stream) {
            sum += x;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(double));
}
BENCHMARK(BM_Read_SingleField_SoA)->Range(10000, 1000000);

static void BM_Read_SingleField_AoS_Baseline(benchmark::State& state) {
    const size_t count = state.range(0);
    std::vector<PhysicsEventAoS> bucket;
    bucket.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        bucket.push_back({i, 1.5, 2.5, 3.5});
    }

    for (auto _ : state) {
        double sum = 0.0;
        for (const auto& event : bucket) {
            sum += event.x;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(PhysicsEventAoS));
}
BENCHMARK(BM_Read_SingleField_AoS_Baseline)->Range(10'000, 1'000'000);

// =============================================================================
// 4. БЕНЧМАРК ОЧИСТКИ (CLEAR & RESET OVERHEAD)
// =============================================================================

static void BM_EventBus_ClearAll(benchmark::State& state) {
    EventSystem::Bus::EventBus bus;
    bus.register_event<BenchmarkSmallSoA>(10000);
    bus.register_event<BenchmarkPhysicsSoA>(10000);

    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < 1000; ++i) {
            bus.emit<BenchmarkSmallSoA>(static_cast<uint32_t>(i), 1.0f);
            bus.emit<BenchmarkPhysicsSoA>(static_cast<uint64_t>(i), 1.0, 2.0, 3.0);
        }
        state.ResumeTiming();

        bus.clear_all();
        benchmark::DoNotOptimize(bus);
    }
}
// Жестко ограничиваем запуск, например, 10 000 итерациями
BENCHMARK(BM_EventBus_ClearAll)->Iterations(10000);

BENCHMARK_MAIN();