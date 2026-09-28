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

#include <cstdint>
#include <cstring>
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
