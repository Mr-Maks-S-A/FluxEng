#include <Terrain/Mesher.hpp>

#include <benchmark/benchmark.h>

namespace {
Terrain::SdfWorld& world() {
    static Terrain::SdfWorld w(1);
    return w;
}
} // namespace

static void BM_MeshOneChunk(benchmark::State& state) {
    for (auto _ : state) benchmark::DoNotOptimize(Terrain::mesh_chunk(world(), {3, 1, 3}));
}
BENCHMARK(BM_MeshOneChunk)->Unit(benchmark::kMillisecond);

static void BM_CarveSphere(benchmark::State& state) {
    Terrain::SdfWorld w(1);
    for (auto _ : state) benchmark::DoNotOptimize(w.carve_sphere(Math::WorldPos::from_meters(60, 20, 60), Math::Fixed::from_int(3)));
}
BENCHMARK(BM_CarveSphere)->Unit(benchmark::kMicrosecond);

static void BM_Raycast(benchmark::State& state) {
    for (auto _ : state)
        benchmark::DoNotOptimize(world().raycast(Math::WorldPos::from_meters(60, 62, 60), {{}, Math::Fixed::from_int(-1), {}}, Math::Fixed::from_int(100)));
}
BENCHMARK(BM_Raycast)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
