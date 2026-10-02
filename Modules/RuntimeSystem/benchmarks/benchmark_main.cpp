/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки RuntimeSystem (google-benchmark).
 *
 * Группы:
 * 1. Цена тика: пустой Runtime против ручного цикла `advance_tick + swap` — накладные расходы самого Runtime.
 * 2. Диспетчеризация модулей: N пустых модулей в тике, с замером времени модулей и без него.
 * 3. Запуск и остановка: initialize() + shutdown() для N модулей (цепочка зависимостей в худшем порядке регистрации).
 * 4. Тик с событиями: отправитель + получатель, тысячи событий за тик через шину.
 * 5. update(): кадр с несколькими тиками (FixedStep + advance_frame).
 *
 * Для осмысленных цифр собирайте в Release. Показатель — время одного тика (items/s = тиков в секунду).
 */

#include <RuntimeSystem/RuntimeSystem.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace rs = RuntimeSystem;
namespace es = EventSystem;

rs::RuntimeConfig bench_config(bool profile = false) {
    return {.ticks_per_second = 60.0, .threads = 0, .tick_arena_bytes = MemorySystem::MiB(8),
            .frame_arena_bytes = MemorySystem::MiB(8), .profile_modules = profile};
}

class Empty final : public rs::Module {
public:
    explicit Empty(std::string name, std::string dependency = {}) : m_name(std::move(name)) {
        if (!dependency.empty()) depends_on(dependency);
    }
    [[nodiscard]] std::string_view name() const noexcept override { return m_name; }
    void tick(rs::Runtime&) override { benchmark::ClobberMemory(); }
private:
    std::string m_name;
};

// -----------------------------------------------------------------------------
// 1. Цена тика
// -----------------------------------------------------------------------------

void BM_Tick_Runtime_NoModules(benchmark::State& state) {
    rs::Runtime rt(bench_config());
    rt.initialize();
    for (auto _ : state) rt.tick();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Tick_Runtime_NoModules);

void BM_Tick_Manual_Baseline(benchmark::State& state) {
    es::EventBus bus;
    auto memory = MemorySystem::DoubleArena::reserve(MemorySystem::MiB(8));
    for (auto _ : state) {
        bus.advance_tick();
        memory.swap();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Tick_Manual_Baseline);

// -----------------------------------------------------------------------------
// 2. Диспетчеризация модулей
// -----------------------------------------------------------------------------

void BM_Tick_Modules(benchmark::State& state) {
    const bool profile = state.range(1) != 0;
    rs::Runtime rt(bench_config(profile));
    for (int i = 0; i < state.range(0); ++i) rt.add<Empty>("M" + std::to_string(i));
    rt.initialize();
    for (auto _ : state) rt.tick();
    state.SetItemsProcessed(state.iterations());
    state.counters["ns/module"] = benchmark::Counter(static_cast<double>(state.range(0)),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_Tick_Modules)->ArgsProduct({{1, 8, 64}, {0, 1}});

// -----------------------------------------------------------------------------
// 3. Запуск и остановка
// -----------------------------------------------------------------------------

void BM_InitializeShutdown_Chain(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    for (auto _ : state) {
        rs::Runtime rt(bench_config());
        // Худший порядок регистрации: каждый модуль зависит от следующего.
        for (int i = 0; i < count; ++i) {
            rt.add<Empty>("M" + std::to_string(i), i + 1 < count ? "M" + std::to_string(i + 1) : std::string{});
        }
        rt.initialize();
        rt.shutdown();
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_InitializeShutdown_Chain)->Arg(10)->Arg(100)->Arg(1000);

// -----------------------------------------------------------------------------
// 4. Тик с событиями
// -----------------------------------------------------------------------------

struct Spark {
    std::uint32_t id = 0;
    float x = 0.0f;
    static constexpr std::string_view event_name = "bench.spark";
    using fields = es::Fields<es::Field<"id", &Spark::id>, es::Field<"x", &Spark::x>>;
};

class SparkSource final : public rs::Module {
public:
    explicit SparkSource(int per_tick) : m_per_tick(per_tick) {}
    [[nodiscard]] std::string_view name() const noexcept override { return "Source"; }
    void declare(rs::Runtime& rt) override {
        m_id = rt.bus().declare_module("Source").produces<Spark>(es::ChannelConfig{.reserve = 1u << 16, .max_events_per_tick = 1u << 20});
    }
    void init(rs::Runtime& rt) override { m_out = rt.bus().writer<Spark>(m_id); }
    void tick(rs::Runtime&) override {
        for (int i = 0; i < m_per_tick; ++i) m_out.emit(Spark{static_cast<std::uint32_t>(i), 1.0f});
    }
private:
    int m_per_tick;
    es::ModuleId m_id{};
    es::EventWriter<Spark> m_out;
};

class SparkSink final : public rs::Module {
public:
    SparkSink() { depends_on("Source"); }
    [[nodiscard]] std::string_view name() const noexcept override { return "Sink"; }
    void declare(rs::Runtime& rt) override { m_id = rt.bus().declare_module("Sink").consumes<Spark>(); }
    void init(rs::Runtime& rt) override { m_in = rt.bus().reader<Spark>(m_id); }
    void tick(rs::Runtime&) override {
        float sum = 0.0f;
        for (const Spark& spark : m_in.events()) sum += spark.x;
        benchmark::DoNotOptimize(sum);
    }
private:
    es::ModuleId m_id{};
    es::EventReader<Spark> m_in;
};

void BM_Tick_EventPipeline(benchmark::State& state) {
    rs::Runtime rt(bench_config());
    rt.add<SparkSink>();
    rt.add<SparkSource>(static_cast<int>(state.range(0)));
    rt.initialize();
    for (auto _ : state) rt.tick();
    state.SetItemsProcessed(state.iterations() * state.range(0)); // события в секунду
}
BENCHMARK(BM_Tick_EventPipeline)->Arg(100)->Arg(10'000)->Arg(100'000);

// -----------------------------------------------------------------------------
// 5. Кадр
// -----------------------------------------------------------------------------

void BM_Update_ThreeTicksPerFrame(benchmark::State& state) {
    rs::Runtime rt(bench_config());
    rt.add<Empty>("M");
    rt.initialize();
    const double frame = 3.0 / 60.0;
    for (auto _ : state) {
        rt.begin_frame(frame);
        benchmark::DoNotOptimize(rt.update(frame));
    }
    state.SetItemsProcessed(state.iterations() * 3);
}
BENCHMARK(BM_Update_ThreeTicksPerFrame);

} // namespace

BENCHMARK_MAIN();
