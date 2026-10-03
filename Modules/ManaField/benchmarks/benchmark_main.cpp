#include <ManaField/ManaField.hpp>

#include <benchmark/benchmark.h>

namespace {
const Math::WorldPos centre = Math::WorldPos::from_meters(64, 32, 64);
}

/// Шаг поля с одной дырой радиуса 8 м (типичный каст): активные чанки + гало.
static void BM_FieldStep(benchmark::State& state) {
    ManaField::ManaGrid field;
    (void)field.draw(centre, Math::Fixed::from_int(8), Math::Mana::from_int(2000));
    for (auto _ : state) {
        field.step();
        benchmark::ClobberMemory();
        state.PauseTiming();
        if (field.allocated_chunks() == 0) (void)field.draw(centre, Math::Fixed::from_int(8), Math::Mana::from_int(2000));
        state.ResumeTiming();
    }
}
BENCHMARK(BM_FieldStep)->Unit(benchmark::kMicrosecond);

/// Дыра на углу восьми чанков: самый дорогой случай (активно 8 чанков + гало).
static void BM_FieldStepCorner(benchmark::State& state) {
    ManaField::ManaGrid field;
    const Math::WorldPos corner = Math::WorldPos::from_meters(32, 32, 32);
    (void)field.draw(corner, Math::Fixed::from_int(8), Math::Mana::from_int(2000));
    for (auto _ : state) {
        field.step();
        benchmark::ClobberMemory();
        state.PauseTiming();
        if (field.allocated_chunks() == 0) (void)field.draw(corner, Math::Fixed::from_int(8), Math::Mana::from_int(2000));
        state.ResumeTiming();
    }
}
BENCHMARK(BM_FieldStepCorner)->Unit(benchmark::kMicrosecond);

static void BM_FieldStepEmpty(benchmark::State& state) {
    ManaField::ManaGrid field;
    for (auto _ : state) field.step();
}
BENCHMARK(BM_FieldStepEmpty)->Unit(benchmark::kNanosecond);

static void BM_FieldDraw(benchmark::State& state) {
    ManaField::ManaGrid field;
    for (auto _ : state) benchmark::DoNotOptimize(field.draw(centre, Math::Fixed::from_int(4), Math::Mana::from_int(10)));
}
BENCHMARK(BM_FieldDraw)->Unit(benchmark::kMicrosecond);

static void BM_FieldHash(benchmark::State& state) {
    ManaField::ManaGrid field;
    (void)field.draw(centre, Math::Fixed::from_int(8), Math::Mana::from_int(2000));
    for (auto _ : state) {
        field.inject(centre, Math::Mana::from_int(1)); // инвалидирует кэш
        benchmark::DoNotOptimize(field.hash());
    }
}
BENCHMARK(BM_FieldHash)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
