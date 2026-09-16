#include <benchmark/benchmark.h>
#include <ECSSystem/World.h>

struct Position {
    float x{0.0f}, y{0.0f};
};

struct Velocity {
    float dx{1.0f}, dy{1.0f};
};

// Бенчмарк массового создания сущностей
static void BM_EntityCreation(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        FluxECS::World world;
        state.ResumeTiming();

        for (int i = 0; i < state.range(0); ++i) {
            auto e = world.create_entity();
            world.add_component<Position>(e, 1.0f, 2.0f);
            benchmark::DoNotOptimize(e);
        }
    }
}
BENCHMARK(BM_EntityCreation)->Range(1000, 8000);

// Бенчмарк итерации системы по линейной памяти (Cache Efficiency)
static void BM_SystemForEach(benchmark::State& state) {
    FluxECS::World world;
    const size_t count = state.range(0);

    for (size_t i = 0; i < count; ++i) {
        auto e = world.create_entity();
        world.add_component<Position>(e, 0.0f, 0.0f);
        if (i % 2 == 0) {
            world.add_component<Velocity>(e, 1.5f, 2.5f);
        }
    }

    for (auto _ : state) {
        world.for_each<Position, Velocity>([](FluxECS::Entity e, Position& pos, const Velocity& vel) {
            pos.x += vel.dx;
            pos.y += vel.dy;
            benchmark::DoNotOptimize(pos);
        });
    }
}
BENCHMARK(BM_SystemForEach)->Range(1000, 8000);

BENCHMARK_MAIN();