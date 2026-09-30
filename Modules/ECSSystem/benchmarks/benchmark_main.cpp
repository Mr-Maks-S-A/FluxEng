/**
 * @file benchmark_main.cpp
 * @brief Бенчмарки ECSSystem (google-benchmark).
 *
 * Группы:
 * 1. Жизненный цикл: create + emplace, destroy.
 * 2. Обход: один компонент, два компонента (плотное и редкое пересечение).
 *    Базовая линия — те же данные в «голых» std::vector (SoA без ECS): столько стоит сама работа.
 * 3. Случайный доступ: get по сущности.
 *
 * Для осмысленных цифр собирайте в Release.
 */

#include <ECSSystem/ECSSystem.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace {

struct Position {
    float x = 0.0f, y = 0.0f;
};
struct Velocity {
    float x = 1.0f, y = 1.0f;
};
struct Burning {
    int ticks = 0;
};

// -----------------------------------------------------------------------------
// 1. Жизненный цикл
// -----------------------------------------------------------------------------

void BM_CreateEmplace(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        ECS::World world;
        world.pool<Position>().reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const ECS::Entity e = world.create();
            world.emplace<Position>(e, 1.0f, 2.0f);
        }
        benchmark::DoNotOptimize(world.alive());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CreateEmplace)->Arg(10'000)->Arg(100'000);

void BM_DestroyRecreate(benchmark::State& state) {
    ECS::World world;
    std::vector<ECS::Entity> entities;
    for (int i = 0; i < 10'000; ++i) {
        entities.push_back(world.create());
        world.emplace<Position>(entities.back());
        world.emplace<Velocity>(entities.back());
    }
    std::size_t cursor = 0;
    for (auto _ : state) {
        world.destroy(entities[cursor]);
        entities[cursor] = world.create();
        world.emplace<Position>(entities[cursor]);
        world.emplace<Velocity>(entities[cursor]);
        cursor = (cursor + 7919) % entities.size();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DestroyRecreate);

// -----------------------------------------------------------------------------
// 2. Обход
// -----------------------------------------------------------------------------

void BM_View1(benchmark::State& state) {
    ECS::World world;
    for (std::int64_t i = 0; i < state.range(0); ++i) world.emplace<Position>(world.create(), 1.0f, 1.0f);
    for (auto _ : state) {
        world.view<Position>().each([](Position& p) { p.x += 1.0f; });
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_View1)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

void BM_Baseline_Vector1(benchmark::State& state) {
    std::vector<Position> positions(static_cast<std::size_t>(state.range(0)), Position{1.0f, 1.0f});
    for (auto _ : state) {
        for (Position& p : positions) p.x += 1.0f;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Baseline_Vector1)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

// Два компонента у каждой сущности: худший случай для поиска во втором пуле.
void BM_View2_Dense(benchmark::State& state) {
    ECS::World world;
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        const ECS::Entity e = world.create();
        world.emplace<Position>(e);
        world.emplace<Velocity>(e);
    }
    for (auto _ : state) {
        world.view<Position, const Velocity>().each([](Position& p, const Velocity& v) {
            p.x += v.x;
            p.y += v.y;
        });
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_View2_Dense)->Arg(10'000)->Arg(100'000);

void BM_Baseline_Vector2(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::vector<Position> positions(n);
    std::vector<Velocity> velocities(n);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            positions[i].x += velocities[i].x;
            positions[i].y += velocities[i].y;
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Baseline_Vector2)->Arg(10'000)->Arg(100'000);

// Редкое пересечение: 100 000 Position, 1% Burning — обход идёт по маленькому пулу.
void BM_View2_Sparse(benchmark::State& state) {
    ECS::World world;
    for (int i = 0; i < 100'000; ++i) {
        const ECS::Entity e = world.create();
        world.emplace<Position>(e);
        if (i % 100 == 0) world.emplace<Burning>(e, 5);
    }
    for (auto _ : state) {
        world.view<Position, Burning>().each([](Position& p, Burning& b) { p.x += static_cast<float>(b.ticks); });
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_View2_Sparse);

// -----------------------------------------------------------------------------
// 3. Случайный доступ
// -----------------------------------------------------------------------------

void BM_GetRandom(benchmark::State& state) {
    ECS::World world;
    std::vector<ECS::Entity> entities;
    for (int i = 0; i < 100'000; ++i) {
        entities.push_back(world.create());
        world.emplace<Position>(entities.back(), static_cast<float>(i), 0.0f);
    }
    std::shuffle(entities.begin(), entities.end(), std::mt19937(1));
    std::size_t cursor = 0;
    for (auto _ : state) {
        Position* p = world.get<Position>(entities[cursor]);
        benchmark::DoNotOptimize(p);
        cursor = cursor + 1 == entities.size() ? 0 : cursor + 1;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GetRandom);

} // namespace

BENCHMARK_MAIN();
