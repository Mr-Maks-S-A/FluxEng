#include <Phases/Schedule.hpp>

#include <benchmark/benchmark.h>

/// Накладные расходы расписания на тик: шесть пустых фаз с замерами.
static void BM_ScheduleRun6(benchmark::State& state) {
    Phases::Schedule schedule;
    for (const char* name : {"commands", "spells", "terrain", "mana", "movement", "events"}) schedule.add(name, [] {});
    for (auto _ : state) schedule.run();
}
BENCHMARK(BM_ScheduleRun6)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
