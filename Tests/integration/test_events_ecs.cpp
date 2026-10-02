/**
 * @file test_events_ecs.cpp
 * @brief EventSystem + ECSSystem: сущности в событиях, поколения, контракты модулей игры.
 *
 * Событие тика N читается в тике N+1. За это время сущность, на которую оно ссылается, может умереть,
 * а её слот — достаться новой. Ссылка ECS::Entity с поколением это видит: `world.valid()` отсекает
 * устаревшие события, и урон не уходит «не тому» существу.
 */

#include <ECSSystem/ECSSystem.hpp>
#include <EventSystem/EventSystem.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace es = EventSystem;

namespace {

struct Hit {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    std::int32_t damage = 0;

    [[nodiscard]] ECS::Entity target() const { return {index, generation}; }

    static constexpr std::string_view event_name = "itest.hit";
    using fields = es::Fields<es::Field<"index", &Hit::index>, es::Field<"generation", &Hit::generation>,
                              es::Field<"damage", &Hit::damage>>;
};

struct Killed {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    static constexpr std::string_view event_name = "itest.killed";
    using fields = es::Fields<es::Field<"index", &Killed::index>, es::Field<"generation", &Killed::generation>>;
};

struct Health {
    int value = 0;
};

Hit hit(ECS::Entity e, int damage) { return Hit{e.index, e.generation, damage}; }

/// Мир и шина «боевой» игры: Combat бьёт, Life — единственный владелец сущностей, Stats считает смерти.
struct Arena {
    ECS::World world;
    es::EventBus bus;
    es::ModuleId combat = bus.declare_module("Combat").produces<Hit>();
    es::ModuleId life = bus.declare_module("Life").consumes<Hit>().produces<Killed>();
    es::ModuleId stats = bus.declare_module("Stats").consumes<Killed>();
    es::EventWriter<Hit> hits_out = bus.writer<Hit>(combat);
    es::EventReader<Hit> hits_in = bus.reader<Hit>(life);
    es::EventWriter<Killed> killed_out = bus.writer<Killed>(life);
    es::EventReader<Killed> killed_in = bus.reader<Killed>(stats);
    int stale_hits = 0;

    void life_tick() {
        for (const Hit& h : hits_in.events()) {
            if (!world.valid(h.target())) {
                ++stale_hits; // цель умерла (и, возможно, её слот уже занят) — событие устарело
                continue;
            }
            Health* health = world.get<Health>(h.target());
            health->value -= h.damage;
            if (health->value <= 0) {
                killed_out.emit(Killed{h.index, h.generation});
                world.destroy(h.target());
            }
        }
    }
};

} // namespace

TEST_SUITE("EventSystem + ECSSystem") {
    TEST_CASE("cpu: a hit sent to a dead entity does not land on the new owner of its slot") {
        Arena a;
        const ECS::Entity goblin = a.world.create();
        a.world.emplace<Health>(goblin, Health{5});

        // Тик 0: два удара по гоблину (3 + 4 — смертельно).
        a.hits_out.emit(hit(goblin, 3));
        a.hits_out.emit(hit(goblin, 4));
        a.bus.advance_tick();

        // Тик 1: Life применяет удары — гоблин погибает; его слот сразу достаётся орку.
        // Combat в этом же тике ещё бьёт «гоблина» — он не знает, что тот уже мёртв.
        a.life_tick();
        CHECK_FALSE(a.world.valid(goblin));
        const ECS::Entity orc = a.world.create();
        a.world.emplace<Health>(orc, Health{10});
        CHECK(orc.index == goblin.index);           // тот же слот…
        CHECK(orc.generation != goblin.generation); // …но другое поколение
        a.hits_out.emit(hit(goblin, 6));
        a.bus.advance_tick();

        // Тик 2: устаревший удар отброшен, орк цел; Stats видит смерть гоблина по старой ссылке.
        a.life_tick();
        CHECK(a.stale_hits == 1);
        CHECK(a.world.get<Health>(orc)->value == 10);
        REQUIRE(a.killed_in.size() == 1);
        const Killed k = a.killed_in.get(0);
        CHECK(ECS::Entity{k.index, k.generation} == goblin);
        CHECK_FALSE(a.world.valid(ECS::Entity{k.index, k.generation}));
        CHECK(a.world.alive() == 1);
    }

    TEST_CASE("cpu: module contracts of the ECS game form an ordered acyclic event graph") {
        Arena a;
        const es::EventGraph graph = a.bus.build_graph();
        const es::ModuleOrder order = graph.module_order();
        CHECK_FALSE(order.has_cycles());
        const auto position = [&](es::ModuleId id) { return std::ranges::find(order.order, id) - order.order.begin(); };
        CHECK(position(a.combat) < position(a.life));
        CHECK(position(a.life) < position(a.stats));
        CHECK(graph.unconsumed_events().empty());
        CHECK(graph.unproduced_events().empty());
        // Писать можно только объявленное: Combat не производит Killed.
        CHECK_THROWS_AS((void)a.bus.writer<Killed>(a.combat), es::EventSystemError);
    }

    TEST_CASE("cpu: entities created from events in one tick are visible to views in the next") {
        Arena a;
        for (int i = 0; i < 100; ++i) {
            const ECS::Entity e = a.world.create();
            a.world.emplace<Health>(e, Health{1 + i % 3});
        }
        // Каждому — по удару в 2: умирают те, у кого 1 или 2 здоровья.
        a.world.view<const Health>().each([&](ECS::Entity e, const Health&) { a.hits_out.emit(hit(e, 2)); });
        a.bus.advance_tick();
        a.life_tick();
        a.bus.advance_tick();
        int survivors = 0;
        a.world.view<const Health>().each([&](const Health& h) {
            CHECK(h.value > 0);
            ++survivors;
        });
        CHECK(survivors == 33);                       // у 33 из 100 было 3 здоровья
        CHECK(a.killed_in.size() == 67);
        CHECK(a.world.alive() == 33);
    }
}
