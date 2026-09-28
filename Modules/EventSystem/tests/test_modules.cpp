/**
 * @file test_modules.cpp
 * @brief Тесты модулей: декларации, проверка контрактов, граф зависимостей.
 */

#include "TestEvents.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

using namespace EventSystem;
using namespace TestEvents;

namespace {

template<typename Range, typename Value>
bool contains(const Range& range, const Value& value) {
    return std::ranges::find(range, value) != std::ranges::end(range);
}

} // namespace

TEST_SUITE("Graph::ModuleRegistry") {
    TEST_CASE("declare and look up modules") {
        ModuleRegistry registry;
        const ModuleId physics = registry.declare("Physics");
        const ModuleId combat = registry.declare("Combat");

        CHECK(physics.index == 0);
        CHECK(combat.index == 1);
        CHECK(registry.size() == 2);
        CHECK(registry.find("Combat") == combat);
        CHECK_FALSE(registry.find("Audio").has_value());
        CHECK(registry.info(physics).name == "Physics");
    }

    TEST_CASE("duplicate or empty names are rejected") {
        ModuleRegistry registry;
        registry.declare("Physics");
        CHECK_THROWS_AS(registry.declare("Physics"), EventSystemError);
        CHECK_THROWS_AS(registry.declare(""), EventSystemError);
    }

    TEST_CASE("productions and consumptions are deduplicated") {
        ModuleRegistry registry;
        const ModuleId module = registry.declare("Physics");
        registry.add_production(module, event_id_v<CollisionEvent>);
        registry.add_production(module, event_id_v<CollisionEvent>);
        registry.add_consumption(module, event_id_v<DamageEvent>);

        CHECK(registry.info(module).produces.size() == 1);
        CHECK(registry.produces(module, event_id_v<CollisionEvent>));
        CHECK_FALSE(registry.produces(module, event_id_v<DamageEvent>));
        CHECK(registry.consumes(module, event_id_v<DamageEvent>));
    }

    TEST_CASE("unknown module ids are rejected") {
        ModuleRegistry registry;
        CHECK_FALSE(registry.contains(ModuleId{}));
        CHECK_FALSE(registry.contains(ModuleId{3}));
        CHECK_THROWS_AS((void)registry.info(ModuleId{3}), EventSystemError);
        CHECK_THROWS_AS(registry.add_production(ModuleId{3}, event_id_v<DamageEvent>), EventSystemError);
        CHECK_FALSE(registry.produces(ModuleId{3}, event_id_v<DamageEvent>));
    }
}

TEST_SUITE("Bus::ModuleBuilder") {
    TEST_CASE("declaring events registers their channels") {
        EventBus bus;
        const ModuleId physics = bus.declare_module("Physics")
                                     .produces<CollisionEvent>(ChannelConfig{.reserve = 64})
                                     .consumes<DamageEvent>();
        CHECK(bus.contains<CollisionEvent>());
        CHECK(bus.contains<DamageEvent>());
        CHECK(bus.modules().produces(physics, event_id_v<CollisionEvent>));
        CHECK(bus.modules().consumes(physics, event_id_v<DamageEvent>));
        CHECK(bus.find(event_id_v<CollisionEvent>)->readable().capacity() >= 64);
    }

    TEST_CASE("checked access enforces the declared contract") {
        EventBus bus;
        const ModuleId physics = bus.declare_module("Physics").produces<CollisionEvent>();
        const ModuleId combat = bus.declare_module("Combat").consumes<CollisionEvent>().produces<DamageEvent>();

        CHECK_NOTHROW((void)bus.writer<CollisionEvent>(physics));
        CHECK_NOTHROW((void)bus.reader<CollisionEvent>(combat));

        // Combat не объявлял, что пишет коллизии.
        CHECK_THROWS_WITH_AS((void)bus.writer<CollisionEvent>(combat),
                             doctest::Contains("did not declare that it produces 'test.collision'"),
                             EventSystemError);
        // Physics не объявлял, что читает урон.
        CHECK_THROWS_WITH_AS((void)bus.reader<DamageEvent>(physics),
                             doctest::Contains("consumes"), EventSystemError);
        CHECK_THROWS_AS((void)bus.writer<CollisionEvent>(ModuleId{}), EventSystemError);
    }

    TEST_CASE("runtime events can be declared by id") {
        EventBus bus;
        const EventSchema& schema = schema_of<HealEvent>();
        bus.register_schema(schema);
        const ModuleId scripts = bus.declare_module("Scripts").produces(schema.id);
        CHECK(bus.modules().produces(scripts, schema.id));

        CHECK_THROWS_AS(bus.declare_module("Broken").consumes(make_event_id("unregistered")), EventSystemError);
    }
}

TEST_SUITE("Graph::EventGraph") {
    // Physics → collision → Combat → damage → Health → death → (никто)
    //                     ↘ Audio
    // Ui читает ui.click, который никто не порождает.
    struct GraphFixture {
        EventBus bus;
        ModuleId physics, combat, audio, health;

        GraphFixture() {
            physics = bus.declare_module("Physics").produces<CollisionEvent>();
            combat = bus.declare_module("Combat").consumes<CollisionEvent>().produces<DamageEvent>();
            audio = bus.declare_module("Audio").consumes<CollisionEvent>();
            health = bus.declare_module("Health").consumes<DamageEvent>().produces<HealEvent>();
            bus.register_event<TickSignalEvent>(); // никем не объявлено
        }
    };

    TEST_CASE_FIXTURE(GraphFixture, "producers and consumers per event") {
        const EventGraph graph = bus.build_graph();
        const EventNode* collision = graph.find_event(event_id_v<CollisionEvent>);
        REQUIRE(collision != nullptr);
        CHECK(collision->producers == std::vector<ModuleId>{physics});
        CHECK(collision->consumers == std::vector<ModuleId>{combat, audio});
        CHECK(graph.find_event(make_event_id("nope")) == nullptr);
    }

    TEST_CASE_FIXTURE(GraphFixture, "dangling events are reported") {
        const ModuleId ui = bus.declare_module("Ui").consumes<VoxelChangedEvent>();
        (void)ui;
        const EventGraph graph = bus.build_graph();

        CHECK(graph.unproduced_events() == std::vector<EventId>{event_id_v<VoxelChangedEvent>});
        CHECK(graph.unconsumed_events() == std::vector<EventId>{event_id_v<HealEvent>});
        CHECK(graph.orphan_events() == std::vector<EventId>{event_id_v<TickSignalEvent>});
    }

    TEST_CASE_FIXTURE(GraphFixture, "module edges") {
        const auto edges = bus.build_graph().module_edges();
        CHECK(edges.size() == 3);
        CHECK(contains(edges, ModuleEdge{.from = physics, .to = combat, .via = event_id_v<CollisionEvent>}));
        CHECK(contains(edges, ModuleEdge{.from = physics, .to = audio, .via = event_id_v<CollisionEvent>}));
        CHECK(contains(edges, ModuleEdge{.from = combat, .to = health, .via = event_id_v<DamageEvent>}));
    }

    TEST_CASE_FIXTURE(GraphFixture, "topological order is deterministic") {
        const ModuleOrder order = bus.build_graph().module_order();
        CHECK_FALSE(order.has_cycles());
        CHECK(order.order == std::vector<ModuleId>{physics, combat, audio, health});
    }

    TEST_CASE("cycles are detected") {
        // A наносит урон, B лечит, A реагирует на лечение → цикл A ↔ B. C от цикла не зависит.
        EventBus cyclic;
        const ModuleId a = cyclic.declare_module("A").produces<DamageEvent>().consumes<HealEvent>();
        const ModuleId b = cyclic.declare_module("B").consumes<DamageEvent>().produces<HealEvent>();
        const ModuleId c = cyclic.declare_module("C").produces<CollisionEvent>();

        const ModuleOrder order = cyclic.build_graph().module_order();
        CHECK(order.has_cycles());
        CHECK(order.order == std::vector<ModuleId>{c});
        CHECK(order.cyclic == std::vector<ModuleId>{a, b});
    }

    TEST_CASE("self loop is not a cycle") {
        EventBus bus;
        bus.declare_module("Echo").produces<DamageEvent>().consumes<DamageEvent>();
        const EventGraph graph = bus.build_graph();
        CHECK(graph.module_edges().empty());
        CHECK_FALSE(graph.module_order().has_cycles());
    }

    TEST_CASE_FIXTURE(GraphFixture, "DOT and text output") {
        const EventGraph graph = bus.build_graph();

        const std::string dot = graph.to_dot();
        CHECK(dot.starts_with("digraph EventGraph {"));
        CHECK(dot.find("label=\"Physics\"") != std::string::npos);
        CHECK(dot.find("label=\"test.collision\"") != std::string::npos);
        CHECK(dot.find("m0 -> e0;") != std::string::npos); // Physics → collision
        CHECK(dot.find("e0 -> m1;") != std::string::npos); // collision → Combat

        const std::string text = graph.to_text();
        INFO(text);
        CHECK(text.find("[Physics]\n  produces test.collision -> Combat, Audio") != std::string::npos);
        CHECK(text.find("consumes test.damage <- Combat") != std::string::npos);
        CHECK(text.find("produces test.heal -> (none)") != std::string::npos);
    }

    TEST_CASE("graph is a snapshot independent of the bus") {
        std::optional<EventGraph> graph;
        {
            EventBus bus;
            bus.declare_module("Physics").produces<CollisionEvent>();
            graph.emplace(bus.build_graph());
        }
        CHECK(graph->modules().size() == 1);
        CHECK(graph->events()[0].name == "test.collision");
    }
}
