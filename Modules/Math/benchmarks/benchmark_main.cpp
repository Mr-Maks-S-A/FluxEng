#include <Math/Math.hpp>

#include <benchmark/benchmark.h>

static void BM_FixedMul(benchmark::State& state) {
    Math::Fixed a = Math::Fixed::from_ratio(3, 7), b = Math::Fixed::from_ratio(5, 3), acc{};
    for (auto _ : state) {
        acc += a * b;
        benchmark::DoNotOptimize(acc);
    }
}
BENCHMARK(BM_FixedMul);

static void BM_Isqrt(benchmark::State& state) {
    std::uint64_t v = 123456789012345ULL;
    for (auto _ : state) benchmark::DoNotOptimize(Math::isqrt(v += 977));
}
BENCHMARK(BM_Isqrt);

static void BM_Fbm4(benchmark::State& state) {
    std::int32_t i = 0;
    for (auto _ : state) {
        i += 513;
        benchmark::DoNotOptimize(Math::fbm(1, Math::Fixed::from_raw(i), Math::Fixed::from_raw(i * 3), 4));
    }
}
BENCHMARK(BM_Fbm4);

BENCHMARK_MAIN();
