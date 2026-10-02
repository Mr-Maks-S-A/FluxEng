/**
 * @file test_jobs_memory_ecs.cpp
 * @brief JobSystem + MemorySystem + ECSSystem + EventSystem: параллельная система над пулом ECS.
 *
 * Схема, как в Swarm и Siege: кусок parallel_for пишет только свои компоненты, временные данные — в арене
 * своего потока (Scheduler::scratch), события — в свой буфер ChunkBuffers; один поток сливает буферы
 * в шину по порядку кусков. Результат (позиции и события) обязан совпадать побитово при любом числе потоков.
 */

#include <ECSSystem/ECSSystem.hpp>
#include <EventSystem/EventSystem.hpp>
#include <JobSystem/JobSystem.hpp>
#include <MemorySystem/MemorySystem.hpp>

#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <string_view>

namespace es = EventSystem;

namespace {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};
struct Velocity {
    float x = 0.0f;
    float y = 0.0f;
};

/// Существо пересекло линию x = 100.
struct Crossed {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    float y = 0.0f;

    static constexpr std::string_view event_name = "itest.crossed";
    using fields = es::Fields<es::Field<"index", &Crossed::index>, es::Field<"generation", &Crossed::generation>,
                              es::Field<"y", &Crossed::y>>;
};

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 1099511628211ULL; }

struct RunResult {
    std::uint64_t positions = 0;
    std::uint64_t events = 0;
    std::size_t crossings = 0;
    std::size_t scratch_left = 0;
};

RunResult simulate(unsigned threads) {
    JobSystem::Scheduler jobs(JobSystem::SchedulerConfig{.threads = threads});
    ECS::World world;
    es::EventBus bus;
    const es::ModuleId movement = bus.declare_module("Movement").produces<Crossed>();
    const es::ModuleId observer = bus.declare_module("Observer").consumes<Crossed>();
    auto out = bus.writer<Crossed>(movement);
    auto in = bus.reader<Crossed>(observer);

    std::uint64_t seed = 42;
    const auto next = [&] {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>(seed >> 40) / static_cast<float>(1ULL << 24);
    };
    for (int i = 0; i < 20000; ++i) {
        const ECS::Entity e = world.create();
        world.emplace<Position>(e, Position{next() * 100.0f, next() * 100.0f});
        world.emplace<Velocity>(e, Velocity{next() * 3.0f - 1.0f, next() * 2.0f - 1.0f});
    }

    RunResult result;
    ECS::ComponentPool<Position>& positions = world.pool<Position>();
    const ECS::ComponentPool<Velocity>& velocities = world.pool<Velocity>();
    const std::size_t count = positions.size();
    constexpr std::size_t grain = 512;
    JobSystem::ChunkBuffers<Crossed> crossed;

    for (int step = 0; step < 12; ++step) {
        crossed.reset(JobSystem::chunk_count(count, grain));
        JobSystem::parallel_for(jobs, count, grain, [&](std::size_t begin, std::size_t end, std::size_t chunk) {
            // Временный массив новых позиций — в арене этого потока; ArenaScope откатит и обнулит его.
            MemorySystem::ArenaScope scope(jobs.scratch());
            auto next_x = scope.arena().push_array<float>(end - begin);
            const auto entities = positions.entities();
            const auto components = positions.components();
            for (std::size_t i = begin; i < end; ++i) {
                const Velocity* v = velocities.get(entities[i]); // чужой пул — только чтение
                next_x[i - begin] = components[i].x + v->x;
            }
            for (std::size_t i = begin; i < end; ++i) {
                Position& p = components[i];
                if (p.x < 100.0f && next_x[i - begin] >= 100.0f) {
                    crossed[chunk].push_back(Crossed{entities[i].index, entities[i].generation, p.y});
                }
                p.x = next_x[i - begin];
                p.y += velocities.get(entities[i])->y;
            }
        });
        crossed.for_each([&](const Crossed& c) { out.emit(c); }); // один поток, порядок кусков
        bus.advance_tick();
        for (const Crossed& c : in.events()) {
            CHECK(world.valid(ECS::Entity{c.index, c.generation}));
            result.events = mix(mix(result.events, c.index), std::bit_cast<std::uint32_t>(c.y));
            ++result.crossings;
        }
    }
    world.view<const Position>().each([&](const Position& p) {
        result.positions = mix(mix(result.positions, std::bit_cast<std::uint32_t>(p.x)), std::bit_cast<std::uint32_t>(p.y));
    });
    result.scratch_left = jobs.scratch().used();
    return result;
}

} // namespace

TEST_SUITE("JobSystem + MemorySystem + ECSSystem + EventSystem") {
    TEST_CASE("cpu: a parallel ECS system gives bit-identical components and events with 0, 1 and 7 threads") {
        const RunResult reference = simulate(0);
        CHECK(reference.crossings > 1000); // система действительно что-то делала
        CHECK(reference.scratch_left == 0); // ArenaScope вернул арену потока
        for (const unsigned threads : {1u, 7u}) {
            CAPTURE(threads);
            const RunResult run = simulate(threads);
            CHECK(run.positions == reference.positions);
            CHECK(run.events == reference.events);
            CHECK(run.crossings == reference.crossings);
            CHECK(run.scratch_left == 0);
        }
    }

    TEST_CASE("cpu: Pool blocks are zeroed, keep stable addresses and are read by a deterministic parallel_reduce") {
        // Pool раздаёт нулевые блоки со стабильными адресами: задачи могут читать их без копий.
        struct Spark {
            float energy = 0.0f;
            std::uint32_t owner = 0;
        };
        MemorySystem::Pool<Spark> pool = MemorySystem::Pool<Spark>::reserve(4096);
        std::vector<Spark*> sparks;
        for (std::uint32_t i = 0; i < 4000; ++i) {
            Spark* s = pool.allocate();
            REQUIRE(s != nullptr);
            CHECK(s->energy == 0.0f); // ZII: блок выдан обнулённым
            *s = Spark{static_cast<float>(i), i};
            sparks.push_back(s);
        }
        JobSystem::Scheduler jobs(JobSystem::SchedulerConfig{.threads = 3});
        const double total = JobSystem::parallel_reduce(
            jobs, sparks.size(), 256, 0.0,
            [&](std::size_t begin, std::size_t end) {
                double sum = 0.0;
                for (std::size_t i = begin; i < end; ++i) sum += sparks[i]->energy;
                return sum;
            },
            [](double a, double b) { return a + b; });
        CHECK(total == doctest::Approx(4000.0 * 3999.0 / 2.0));
        for (Spark* s : sparks) pool.free(s);
        CHECK(pool.live() == 0);
    }
}
