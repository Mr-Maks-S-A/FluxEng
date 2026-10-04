#include <Challenge/Play.hpp>

#include <benchmark/benchmark.h>

/// Судья на каждом тике: сколько стоит `observe` (чтение мира: позиция, мана, гримуар, одна выборка SDF).
static void BM_RefereeObserve(benchmark::State& state) {
    const Challenge::Level& level = *Challenge::find_level("fort");
    SpellSim::Simulation sim(level.config);
    Challenge::install_library(level, sim);
    sim.tick({});
    Challenge::Referee referee(level);
    for (auto _ : state) {
        referee.observe(sim);
        Challenge::Metrics metrics = referee.metrics();
        benchmark::DoNotOptimize(metrics);
        // Судья замирает после исхода; для замера чистого `observe` сбрасываем его, пока идёт цикл.
        if (referee.status() != Challenge::Status::Running) referee = Challenge::Referee(level);
    }
}
BENCHMARK(BM_RefereeObserve);

/// Тик симуляции для сравнения: судья должен быть ничтожен рядом с ним.
static void BM_SimTick(benchmark::State& state) {
    const Challenge::Level& level = *Challenge::find_level("walk");
    SpellSim::Simulation sim(level.config);
    for (auto _ : state) sim.tick({});
}
BENCHMARK(BM_SimTick);

/// Прохождение уровня целиком эталонным решением (мир + судья + бот): стоимость одной попытки игрока в тесте и в ИИ.
static void BM_PlayReference(benchmark::State& state) {
    const Challenge::Level& level = *Challenge::find_level(state.range(0) == 0 ? "mine" : "fort");
    const Challenge::Solution& solution = *Challenge::reference_solution(level.id);
    std::uint32_t ticks = 0;
    for (auto _ : state) {
        const Challenge::Outcome o = Challenge::play(level, solution);
        ticks = o.ticks_run;
        int stars = o.stars;
        benchmark::DoNotOptimize(stars);
    }
    state.counters["ticks"] = ticks;
}
BENCHMARK(BM_PlayReference)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond);

/// Создание мира уровня: сид + правки поверх (у «крепости» их около 80 шаров). Это цена старта/перезапуска уровня.
static void BM_LevelWorldSetup(benchmark::State& state) {
    const Challenge::Level& level = *Challenge::find_level(state.range(0) == 0 ? "mine" : "fort");
    for (auto _ : state) {
        SpellSim::Simulation sim(level.config);
        benchmark::DoNotOptimize(sim.tick_number());
    }
    state.counters["edits"] = static_cast<double>(level.config.setup.size());
}
BENCHMARK(BM_LevelWorldSetup)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
