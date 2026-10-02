/**
 * @file test_tick_memory.cpp
 * @brief MemorySystem::DoubleArena + EventSystem: память тика живёт ровно столько же, сколько событие.
 *
 * Событие тика N видно в тике N+1 и исчезает после него. DoubleArena устроена так же: выделенное
 * в тике N читается в N+1 через previous() и обнуляется следующим swap(). Поэтому событие может
 * нести ссылку на данные в памяти тика (путь, список целей) — без копий и без утечек.
 * Порядок как в Core::App: тик игры → bus.advance_tick() → arenas.swap().
 */

#include <EventSystem/EventSystem.hpp>
#include <MemorySystem/MemorySystem.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace es = EventSystem;

namespace {

/// «Путь готов»: адрес массива точек в памяти тика и их число.
struct PathReady {
    std::uint64_t address = 0;
    std::uint32_t count = 0;

    [[nodiscard]] std::span<const std::int32_t> points() const {
        return {reinterpret_cast<const std::int32_t*>(static_cast<std::uintptr_t>(address)), count};
    }

    static constexpr std::string_view event_name = "itest.path_ready";
    using fields = es::Fields<es::Field<"address", &PathReady::address>, es::Field<"count", &PathReady::count>>;
};

} // namespace

TEST_SUITE("MemorySystem + EventSystem") {
    TEST_CASE("cpu: data referenced by an event is valid for exactly the tick the event is visible") {
        es::EventBus bus;
        const es::ModuleId planner = bus.declare_module("Planner").produces<PathReady>();
        const es::ModuleId walker = bus.declare_module("Walker").consumes<PathReady>();
        auto out = bus.writer<PathReady>(planner);
        auto in = bus.reader<PathReady>(walker);
        MemorySystem::DoubleArena arenas = MemorySystem::DoubleArena::reserve(MemorySystem::MiB(1));

        std::span<std::int32_t> previous_points;
        for (int tick = 0; tick < 6; ++tick) {
            CAPTURE(tick);
            // Walker: путь прошлого тика читается из previous() и цел.
            if (tick > 0) {
                REQUIRE(in.size() == 1);
                const PathReady path = in.get(0);
                CHECK(arenas.previous().owns(path.points().data()));
                for (std::uint32_t i = 0; i < path.count; ++i) CHECK(path.points()[i] == (tick - 1) * 100 + static_cast<int>(i));
            } else {
                CHECK(in.empty());
            }
            // Память позапрошлого тика уже очищена swap(): ZII — нули, а не мусор.
            if (tick >= 2) {
                for (const std::int32_t value : previous_points) CHECK(value == 0);
            }

            // Planner: новый путь в памяти текущего тика.
            std::span<std::int32_t> points = arenas.current().push_array<std::int32_t>(16);
            for (std::size_t i = 0; i < points.size(); ++i) {
                CHECK(points[i] == 0); // арена отдаёт нулевую память
                points[i] = tick * 100 + static_cast<int>(i);
            }
            out.emit(PathReady{static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(points.data())),
                               static_cast<std::uint32_t>(points.size())});
            if (tick >= 1) {
                // Сохраняем ссылку на путь прошлого тика: после swap() в конце этого тика он обнулится.
                previous_points = std::span<std::int32_t>(
                    const_cast<std::int32_t*>(in.get(0).points().data()), in.get(0).count);
            }

            bus.advance_tick();
            arenas.swap();
        }
    }

    TEST_CASE("cpu: arena scopes nest inside a tick and leave no garbage") {
        MemorySystem::Arena arena = MemorySystem::Arena::reserve(MemorySystem::MiB(1));
        const std::size_t before = arena.used();
        {
            MemorySystem::ArenaScope outer(arena);
            auto a = arena.push_array<std::uint64_t>(100);
            a[0] = 7;
            {
                MemorySystem::ArenaScope inner(arena);
                auto b = arena.push_array<std::uint64_t>(1000);
                b[999] = 9;
            }
            auto c = arena.push_array<std::uint64_t>(1000); // переиспользует место inner — и оно снова нулевое
            CHECK(c[999] == 0);
            CHECK(a[0] == 7);
        }
        CHECK(arena.used() == before);
    }
}
