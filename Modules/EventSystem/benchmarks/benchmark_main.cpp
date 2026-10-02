/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки EventSystem (google-benchmark).
 *
 * Группы:
 * 1. Запись: типизированный emit в AoS/SoA, сырой emit_raw, базовая линия std::vector.
 * 2. Чтение: все поля и одно поле, AoS против SoA. Честное сравнение раскладок:
 *    SoA выигрывает на одном поле и проигрывает или равна AoS на всех полях.
 * 3. Накладные расходы шины: advance_tick на многих каналах, получение писателя.
 *
 * Запуск: `EventSystemBenchmarks --benchmark_filter=Read`.
 * Для осмысленных цифр собирайте в Release.
 */

#include <EventSystem/EventSystem.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace es = EventSystem;

// =============================================================================
// События для замеров: одинаковые данные, разная раскладка.
// =============================================================================

struct PhysicsAoS {
    std::uint64_t entity = 0;
    double x = 0.0, y = 0.0, z = 0.0;

    static constexpr std::string_view event_name = "bench.physics_aos";
    using fields = es::Fields<
        es::Field<"entity", &PhysicsAoS::entity>,
        es::Field<"x", &PhysicsAoS::x>,
        es::Field<"y", &PhysicsAoS::y>,
        es::Field<"z", &PhysicsAoS::z>>;
};

struct PhysicsSoA {
    std::uint64_t entity = 0;
    double x = 0.0, y = 0.0, z = 0.0;

    static constexpr std::string_view event_name = "bench.physics_soa";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<
        es::Field<"entity", &PhysicsSoA::entity>,
        es::Field<"x", &PhysicsSoA::x>,
        es::Field<"y", &PhysicsSoA::y>,
        es::Field<"z", &PhysicsSoA::z>>;
};

/// Типичное событие вокселей: 16 байт, массовое.
struct VoxelChanged {
    std::int32_t x = 0, y = 0, z = 0;
    std::uint32_t block = 0;

    static constexpr std::string_view event_name = "bench.voxel_changed";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<
        es::Field<"x", &VoxelChanged::x>,
        es::Field<"y", &VoxelChanged::y>,
        es::Field<"z", &VoxelChanged::z>,
        es::Field<"block", &VoxelChanged::block>>;
};

template<typename E>
E make_physics(std::uint64_t i) {
    const double d = static_cast<double>(i);
    return E{.entity = i, .x = d, .y = d * 0.5, .z = d * 0.25};
}

/// Шина с одним каналом `E`, в которой уже видно `count` событий.
template<typename E>
es::EventBus make_filled_bus(std::size_t count) {
    es::EventBus bus;
    bus.register_event<E>(es::ChannelConfig{.reserve = count});
    auto writer = bus.writer<E>();
    for (std::size_t i = 0; i < count; ++i) {
        writer.emit(make_physics<E>(i));
    }
    bus.advance_tick();
    return bus;
}

constexpr std::int64_t min_events = 1 << 10;
constexpr std::int64_t max_events = 1 << 18;

// =============================================================================
// 1. Запись
// =============================================================================

void BM_Baseline_VectorPushBack(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::vector<PhysicsAoS> events;
    events.reserve(count);
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            events.push_back(make_physics<PhysicsAoS>(i));
        }
        benchmark::DoNotOptimize(events.data());
        events.clear();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Baseline_VectorPushBack)->Range(min_events, max_events);

template<typename E>
void BM_Emit(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    es::EventBus bus;
    bus.register_event<E>(es::ChannelConfig{.reserve = count});
    auto writer = bus.writer<E>();
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            writer.emit(make_physics<E>(i));
        }
        bus.advance_tick(); // O(1): обмен буферов без аллокаций
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Emit<PhysicsAoS>)->Name("BM_Emit_AoS")->Range(min_events, max_events);
BENCHMARK(BM_Emit<PhysicsSoA>)->Name("BM_Emit_SoA")->Range(min_events, max_events);

template<typename E>
void BM_EmitRaw(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    es::EventBus bus;
    es::IChannel& channel = bus.register_event<E>(es::ChannelConfig{.reserve = count});
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            const E event = make_physics<E>(i);
            channel.emit_raw(reinterpret_cast<const std::byte*>(&event));
        }
        bus.advance_tick();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_EmitRaw<PhysicsAoS>)->Name("BM_EmitRaw_AoS")->Range(min_events, max_events);
BENCHMARK(BM_EmitRaw<PhysicsSoA>)->Name("BM_EmitRaw_SoA")->Range(min_events, max_events);

void BM_Emit_Unreserved(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        es::EventBus bus;
        bus.register_event<VoxelChanged>();
        auto writer = bus.writer<VoxelChanged>();
        state.ResumeTiming();

        for (std::size_t i = 0; i < count; ++i) {
            const auto v = static_cast<std::int32_t>(i);
            writer.emit(VoxelChanged{.x = v, .y = v, .z = v, .block = 1});
        }
        benchmark::DoNotOptimize(writer.pending_count());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Emit_Unreserved)->Range(min_events, max_events);

// =============================================================================
// 2. Чтение
// =============================================================================

void BM_ReadAllFields_AoS(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    es::EventBus bus = make_filled_bus<PhysicsAoS>(count);
    const auto reader = bus.reader<PhysicsAoS>();
    for (auto _ : state) {
        double sum = 0.0;
        for (const PhysicsAoS& e : reader.events()) {
            sum += static_cast<double>(e.entity) + e.x + e.y + e.z;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.SetBytesProcessed(state.iterations() * state.range(0) * static_cast<std::int64_t>(sizeof(PhysicsAoS)));
}
BENCHMARK(BM_ReadAllFields_AoS)->Range(min_events, max_events);

void BM_ReadAllFields_SoA(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    es::EventBus bus = make_filled_bus<PhysicsSoA>(count);
    const auto reader = bus.reader<PhysicsSoA>();
    for (auto _ : state) {
        const auto entity = reader.column<&PhysicsSoA::entity>();
        const auto xs = reader.column<&PhysicsSoA::x>();
        const auto ys = reader.column<&PhysicsSoA::y>();
        const auto zs = reader.column<&PhysicsSoA::z>();
        double sum = 0.0;
        for (std::size_t i = 0; i < reader.size(); ++i) {
            sum += static_cast<double>(entity[i]) + xs[i] + ys[i] + zs[i];
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.SetBytesProcessed(state.iterations() * state.range(0) * static_cast<std::int64_t>(sizeof(PhysicsSoA)));
}
BENCHMARK(BM_ReadAllFields_SoA)->Range(min_events, max_events);

void BM_ReadAllFields_SoA_ForEach(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    es::EventBus bus = make_filled_bus<PhysicsSoA>(count);
    const auto reader = bus.reader<PhysicsSoA>();
    for (auto _ : state) {
        double sum = 0.0;
        reader.for_each([&](const PhysicsSoA& e) { sum += static_cast<double>(e.entity) + e.x + e.y + e.z; });
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ReadAllFields_SoA_ForEach)->Range(min_events, max_events);

void BM_ReadOneField_AoS(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    es::EventBus bus = make_filled_bus<PhysicsAoS>(count);
    const auto reader = bus.reader<PhysicsAoS>();
    for (auto _ : state) {
        double sum = 0.0;
        for (const PhysicsAoS& e : reader.events()) {
            sum += e.x;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ReadOneField_AoS)->Range(min_events, max_events);

void BM_ReadOneField_SoA(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    es::EventBus bus = make_filled_bus<PhysicsSoA>(count);
    const auto reader = bus.reader<PhysicsSoA>();
    for (auto _ : state) {
        double sum = 0.0;
        for (const double x : reader.column<&PhysicsSoA::x>()) {
            sum += x;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ReadOneField_SoA)->Range(min_events, max_events);

void BM_ReadOneField_Raw(benchmark::State& state) {
    // Путь скриптов: поле ищется по имени один раз, затем читается через field_data().
    const auto count = static_cast<std::size_t>(state.range(0));
    es::EventBus bus = make_filled_bus<PhysicsSoA>(count);
    const es::EventBuffer& buffer = bus.find("bench.physics_soa")->readable();
    const std::size_t field = *buffer.schema().field_index("x");
    for (auto _ : state) {
        double sum = 0.0;
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            double x = 0.0;
            std::memcpy(&x, buffer.field_data(i, field), sizeof(double));
            sum += x;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ReadOneField_Raw)->Range(min_events, max_events);

// =============================================================================
// 3. Накладные расходы шины
// =============================================================================

// =============================================================================
// Дорожки потоков: запись без блокировок и слияние в порядке дорожек
// =============================================================================

/// Постоянные потоки для замера (создавать потоки на каждую итерацию — значит мерить создание потоков).
/// Атомики здесь только раздают куски работы; запись событий в дорожки — без синхронизации.
class BenchPool {
public:
    explicit BenchPool(unsigned threads) {
        for (unsigned t = 0; t < threads; ++t) {
            m_workers.emplace_back([this](std::stop_token stop) {
                std::uint64_t seen = 0;
                while (!stop.stop_requested()) {
                    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
                    if (generation == seen) {
                        std::this_thread::yield();
                        continue;
                    }
                    seen = generation;
                    drain();
                    m_done.fetch_add(1, std::memory_order_acq_rel);
                }
            });
        }
    }
    ~BenchPool() {
        for (auto& w : m_workers) w.request_stop();
    }

    /// Выполняет job(chunk) для chunk в [0, chunks) всеми потоками; возвращается, когда всё сделано.
    void run(std::size_t chunks, const std::function<void(std::size_t)>& job) {
        m_job = &job;
        m_chunks = chunks;
        m_next.store(0, std::memory_order_relaxed);
        m_done.store(0, std::memory_order_relaxed);
        m_generation.fetch_add(1, std::memory_order_acq_rel);
        while (m_done.load(std::memory_order_acquire) < m_workers.size()) std::this_thread::yield();
    }

private:
    void drain() {
        for (std::size_t c = m_next.fetch_add(1); c < m_chunks; c = m_next.fetch_add(1)) (*m_job)(c);
    }
    std::vector<std::jthread> m_workers;
    const std::function<void(std::size_t)>* m_job = nullptr;
    std::size_t m_chunks = 0;
    std::atomic<std::size_t> m_next{0};
    std::atomic<std::size_t> m_done{0};
    std::atomic<std::uint64_t> m_generation{0};
};

/// Один поток пишет через emit() — эталон.
void BM_Lanes_SingleEmit(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    es::EventBus bus;
    bus.register_event<PhysicsSoA>(es::ChannelConfig{.reserve = count});
    auto writer = bus.writer<PhysicsSoA>();
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) writer.emit(make_physics<PhysicsSoA>(i));
        bus.advance_tick();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Lanes_SingleEmit)->Arg(1 << 20)->Unit(benchmark::kMicrosecond);

/// N потоков пишут SoA-события в свои дорожки; в замер входит слияние (advance_tick).
void BM_Lanes_Threads(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto threads = static_cast<unsigned>(state.range(1));
    constexpr std::size_t grain = 16 * 1024;
    const std::size_t chunks = (count + grain - 1) / grain;
    es::EventBus bus;
    bus.register_event<PhysicsSoA>(es::ChannelConfig{.reserve = count});
    auto writer = bus.writer<PhysicsSoA>();
    BenchPool pool(threads);
    for (auto _ : state) {
        auto lanes = writer.lanes(chunks);
        pool.run(chunks, [&](std::size_t chunk) {
            const std::size_t end = std::min(count, (chunk + 1) * grain);
            for (std::size_t i = chunk * grain; i < end; ++i) lanes.emit(chunk, make_physics<PhysicsSoA>(i));
        });
        bus.advance_tick(); // слияние дорожек: memcpy на колонку
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Lanes_Threads)->Args({1 << 20, 1})->Args({1 << 20, 4})->Args({1 << 20, 8})->Unit(benchmark::kMicrosecond)->UseRealTime();

/// Типичная система: на элемент — вычисление (проверка попадания), событие — у каждого восьмого.
/// Здесь работа, а не запись в память, — главная цена, и дорожки дают масштабирование.
std::uint64_t hit_test(std::size_t i) noexcept {
    std::uint64_t h = i * 0x9E3779B97F4A7C15ull;
    for (int k = 0; k < 32; ++k) h ^= (h << 13) ^ (h >> 7) ^ static_cast<std::uint64_t>(k);
    return h;
}

void BM_Lanes_Work(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto threads = static_cast<unsigned>(state.range(1)); // 0 — без потоков, обычный emit
    constexpr std::size_t grain = 16 * 1024;
    const std::size_t chunks = (count + grain - 1) / grain;
    es::EventBus bus;
    bus.register_event<PhysicsSoA>(es::ChannelConfig{.reserve = count});
    auto writer = bus.writer<PhysicsSoA>();
    std::unique_ptr<BenchPool> pool = threads > 0 ? std::make_unique<BenchPool>(threads) : nullptr;
    for (auto _ : state) {
        if (!pool) {
            for (std::size_t i = 0; i < count; ++i)
                if (const auto h = hit_test(i); (h & 7) == 0) writer.emit(make_physics<PhysicsSoA>(h));
        } else {
            auto lanes = writer.lanes(chunks);
            pool->run(chunks, [&](std::size_t chunk) {
                const std::size_t end = std::min(count, (chunk + 1) * grain);
                for (std::size_t i = chunk * grain; i < end; ++i)
                    if (const auto h = hit_test(i); (h & 7) == 0) lanes.emit(chunk, make_physics<PhysicsSoA>(h));
            });
        }
        bus.advance_tick();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Lanes_Work)->Args({1 << 20, 0})->Args({1 << 20, 1})->Args({1 << 20, 4})->Args({1 << 20, 8})->Unit(benchmark::kMicrosecond)->UseRealTime();

void BM_AdvanceTick(benchmark::State& state) {
    // Много каналов, в каждом немного событий: стоимость смены тика.
    const auto channels = static_cast<std::size_t>(state.range(0));
    es::EventBus bus;
    std::vector<es::EventSchema> schemas;
    schemas.reserve(channels);
    for (std::size_t c = 0; c < channels; ++c) {
        es::EventSchema schema = es::schema_of<VoxelChanged>();
        schema.name = "bench.channel_" + std::to_string(c);
        schema.id = es::make_event_id(schema.name);
        bus.register_schema(schema, es::ChannelConfig{.reserve = 16});
    }
    const VoxelChanged event{};
    for (auto _ : state) {
        for (std::size_t c = 0; c < channels; ++c) {
            bus.channel_at(c).emit_raw(reinterpret_cast<const std::byte*>(&event));
        }
        bus.advance_tick();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_AdvanceTick)->Range(8, 1024);

void BM_AcquireWriter(benchmark::State& state) {
    // Цена получения писателя: поиск канала + сравнение схем.
    // Поэтому писатель получают один раз, а не на каждый emit.
    es::EventBus bus;
    bus.register_event<PhysicsSoA>();
    for (auto _ : state) {
        auto writer = bus.writer<PhysicsSoA>();
        benchmark::DoNotOptimize(writer);
    }
}
BENCHMARK(BM_AcquireWriter);

} // namespace

BENCHMARK_MAIN();
