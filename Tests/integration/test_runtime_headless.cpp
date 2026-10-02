/**
 * @file test_runtime_headless.cpp
 * @brief RuntimeSystem + ECSSystem + JobSystem + EventSystem + MemorySystem: «выделенный сервер» без окна и GPU.
 *
 * Три модуля с зависимостями: World (единственный владелец структуры ECS), Movement (параллельная система,
 * события кусков — через дорожки шины), Tally (читатель событий). Проверяется то, что видно только вместе:
 * - порядок init задаёт зависимость, а не порядок add();
 * - итог симуляции (контрольная сумма позиций и событий) побитово одинаков при 0, 1 и 4 потоках;
 * - память тика: данные тика N живы в N+1;
 * - исключение посреди работы не оставляет модули без shutdown().
 */

#include <ECSSystem/ECSSystem.hpp>
#include <RuntimeSystem/RuntimeSystem.hpp>

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace es = EventSystem;
namespace rs = RuntimeSystem;
namespace js = JobSystem;

namespace {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};
struct Velocity {
    float x = 0.0f;
    float y = 0.0f;
};

struct Crossed {
    std::uint32_t index = 0;
    std::uint32_t tick = 0;
    static constexpr std::string_view event_name = "headless.crossed";
    using fields = es::Fields<es::Field<"index", &Crossed::index>, es::Field<"tick", &Crossed::tick>>;
};

constexpr std::size_t kEntities = 20'000;
constexpr float kLine = 50.0f;

class World final : public rs::Module {
public:
    std::string_view name() const noexcept override { return "World"; }
    void init(rs::Runtime&) override {
        for (std::size_t i = 0; i < kEntities; ++i) {
            const ECS::Entity e = ecs.create();
            const auto f = static_cast<float>(i);
            ecs.emplace<Position>(e, Position{std::fmod(f * 0.37f, 100.0f), 0.0f});
            ecs.emplace<Velocity>(e, Velocity{0.5f + std::fmod(f * 0.013f, 2.0f), 0.0f});
        }
    }
    void shutdown(rs::Runtime&) noexcept override { ecs.clear(); }
    ECS::World ecs;
};

class Movement final : public rs::Module {
public:
    explicit Movement(World& world) : m_world(world) { depends_on("World"); }
    std::string_view name() const noexcept override { return "Movement"; }
    void declare(rs::Runtime& rt) override { m_id = rt.bus().declare_module("Movement").produces<Crossed>(); }
    void init(rs::Runtime& rt) override { m_out = rt.bus().writer<Crossed>(m_id); }
    void tick(rs::Runtime& rt) override {
        auto positions = m_world.ecs.pool<Position>().components();
        auto velocities = m_world.ecs.pool<Velocity>().components();
        const auto tick = static_cast<std::uint32_t>(rt.current_tick());
        constexpr std::size_t grain = 1024;
        auto lanes = m_out.lanes(js::chunk_count(positions.size(), grain)); // до работы, в главном потоке
        js::parallel_for(rt.jobs(), positions.size(), grain, [&](std::size_t begin, std::size_t end, std::size_t chunk) {
            for (std::size_t i = begin; i < end; ++i) {
                const float before = positions[i].x;
                positions[i].x += velocities[i].x;
                if (before < kLine && positions[i].x >= kLine) lanes.emit(chunk, Crossed{static_cast<std::uint32_t>(i), tick});
            }
        });
    }

private:
    World& m_world;
    es::ModuleId m_id{};
    es::EventWriter<Crossed> m_out;
};

class Tally final : public rs::Module {
public:
    Tally() { depends_on("Movement"); }
    std::string_view name() const noexcept override { return "Tally"; }
    void declare(rs::Runtime& rt) override { m_id = rt.bus().declare_module("Tally").consumes<Crossed>(); }
    void init(rs::Runtime& rt) override { m_in = rt.bus().reader<Crossed>(m_id); }
    void tick(rs::Runtime& rt) override {
        for (const Crossed& c : m_in.events()) {
            ++crossings;
            checksum = (checksum ^ (static_cast<std::uint64_t>(c.index) << 20 ^ c.tick)) * 1099511628211ull;
        }
        // Память тика: пишем в текущую, читаем прошлую — как события шины.
        auto* now = rt.tick_arena().push<std::uint32_t>();
        *now = static_cast<std::uint32_t>(rt.current_tick());
        if (last != nullptr && !(rt.previous_tick_arena().owns(last) && *last + 1 == *now)) tick_memory_ok = false;
        last = now;
    }
    std::uint64_t checksum = 1469598103934665603ull;
    std::uint64_t crossings = 0;
    bool tick_memory_ok = true;

private:
    es::ModuleId m_id{};
    es::EventReader<Crossed> m_in;
    std::uint32_t* last = nullptr;
};

struct Outcome {
    std::uint64_t checksum = 0;
    std::uint64_t crossings = 0;
    std::uint64_t positions = 0;
    bool tick_memory_ok = false;
    std::vector<std::string_view> order;
};

Outcome simulate(int threads, int ticks) {
    rs::Runtime server({.ticks_per_second = 60.0, .threads = threads, .tick_arena_bytes = MemorySystem::MiB(16),
                        .frame_arena_bytes = MemorySystem::MiB(4)});
    // Порядок add() намеренно «неправильный»: сначала самый зависимый. Порядок init задают depends_on().
    Tally& tally = server.add<Tally>();
    World& world = server.add<World>();
    server.add<Movement>(world);
    server.run({.max_ticks = ticks, .realtime = false});

    Outcome out;
    out.checksum = tally.checksum;
    out.crossings = tally.crossings;
    out.tick_memory_ok = tally.tick_memory_ok;
    out.order = server.initialization_order();
    out.positions = 1469598103934665603ull;
    for (const Position& p : world.ecs.pool<Position>().components()) {
        out.positions = (out.positions ^ std::bit_cast<std::uint32_t>(p.x)) * 1099511628211ull;
    }
    return out;
}

} // namespace

TEST_SUITE("RuntimeSystem + ECS + JobSystem + EventSystem (headless)") {
    TEST_CASE("cpu: headless server — init order follows dependencies, tick memory and events work without a window") {
        const Outcome out = simulate(0, 200);
        CHECK(out.order == std::vector<std::string_view>{"World", "Movement", "Tally"});
        CHECK(out.crossings > 0);
        CHECK(out.tick_memory_ok);
    }

    TEST_CASE("cpu: headless server — result is bit-identical at 0, 1 and 4 threads") {
        const Outcome reference = simulate(0, 200);
        for (const int threads : {1, 4}) {
            CAPTURE(threads);
            const Outcome other = simulate(threads, 200);
            CHECK(other.crossings == reference.crossings);
            CHECK(other.checksum == reference.checksum);
            CHECK(other.positions == reference.positions);
        }
    }

    TEST_CASE("cpu: headless server — an exception mid-simulation still shuts every module down, in reverse order") {
        struct Bomb final : rs::Module {
            explicit Bomb(std::vector<std::string>& log) : m_log(log) { depends_on("World"); }
            std::string_view name() const noexcept override { return "Bomb"; }
            void tick(rs::Runtime& rt) override { if (rt.current_tick() == 5) throw std::runtime_error("boom"); }
            void shutdown(rs::Runtime&) noexcept override { m_log.push_back("Bomb.shutdown"); }
            std::vector<std::string>& m_log;
        };
        struct Base final : rs::Module {
            explicit Base(std::vector<std::string>& log) : m_log(log) {}
            std::string_view name() const noexcept override { return "World"; }
            void shutdown(rs::Runtime&) noexcept override { m_log.push_back("World.shutdown"); }
            std::vector<std::string>& m_log;
        };
        std::vector<std::string> log;
        {
            rs::Runtime server({.threads = 0, .tick_arena_bytes = MemorySystem::MiB(4), .frame_arena_bytes = MemorySystem::MiB(4)});
            server.add<Bomb>(log);
            server.add<Base>(log);
            CHECK_THROWS_AS(server.run({.max_ticks = 100, .realtime = false}), std::runtime_error);
            CHECK(server.current_tick() == 5);
            CHECK(log.empty()); // исключение ещё летит из run(); завершение — при shutdown()/деструкторе
        }
        CHECK(log == std::vector<std::string>{"Bomb.shutdown", "World.shutdown"});
    }
}
