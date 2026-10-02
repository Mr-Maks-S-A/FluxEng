/**
 * @file test_trace.cpp
 * @brief Дерево причин: событие → следствия → следствия следствий, через тики, каналы и дорожки потоков.
 */

#include "TestEvents.hpp"

#include <EventSystem/EventSystem.hpp>

#include <doctest/doctest.h>

#include <string>

using namespace EventSystem;
using namespace TestEvents;

TEST_SUITE("Cause tree") {
    TEST_CASE("EventRef packs channel, time and index; zero means no cause") {
        const EventRef ref = EventRef::make(12345, 7, 999);
        CHECK(ref.valid());
        CHECK(ref.time() == 12345);
        CHECK(ref.channel() == 7);
        CHECK(ref.index() == 999);
        CHECK_FALSE(EventRef{}.valid());
        CHECK(EventRef::make(0, 0, 0).valid()); // первое событие первого канала — не «нет причины»
    }

    TEST_CASE("collision → damage → heal: the chain is recovered from the journal") {
        EventBus bus;
        bus.register_event<CollisionEvent>(ChannelConfig{.trace = true});
        bus.register_event<DamageEvent>(ChannelConfig{.trace = true});
        bus.register_event<HealEvent>(ChannelConfig{.trace = true});
        auto collisions = bus.writer<CollisionEvent>();
        auto collisions_in = bus.reader<CollisionEvent>();
        auto damage = bus.writer<DamageEvent>();
        auto damage_in = bus.reader<DamageEvent>();
        auto heal = bus.writer<HealEvent>();
        auto heal_in = bus.reader<HealEvent>();

        collisions.emit({1, 2, {}});            // корень (ввод, физика…)
        collisions.emit({3, 4, {}});
        bus.advance_tick();
        REQUIRE(collisions_in.size() == 2);
        const EventRef root = collisions_in.ref(1);
        damage.emit({4, 10.0f}, root);          // урон — из-за второго столкновения
        damage.emit({4, 3.0f}, root);
        bus.advance_tick();
        REQUIRE(damage_in.size() == 2);
        CHECK(damage_in.cause(0) == root);
        heal.emit({4, 5.0f}, damage_in.ref(1)); // лечение — реакция на второй урон
        bus.advance_tick();
        REQUIRE(heal_in.size() == 1);

        const auto chain = bus.cause_chain(heal_in.ref(0));
        REQUIRE(chain.size() == 3);
        CHECK(chain[0].ref == root);
        CHECK(chain[0].event == event_id_v<CollisionEvent>);
        CHECK(chain[1].event == event_id_v<DamageEvent>);
        CHECK(chain[2].event == event_id_v<HealEvent>);

        CHECK(bus.effects(root).size() == 2);
        const std::string tree = bus.trace_tree(root);
        CHECK(tree.find("test.collision@1#1") == 0);
        CHECK(tree.find("  test.damage@2#0") != std::string::npos);
        CHECK(tree.find("    test.heal@3#0") != std::string::npos);
    }

    TEST_CASE("causes travel through thread lanes and scheduled events") {
        EventBus bus;
        bus.register_event<DamageEvent>(ChannelConfig{.trace = true});
        bus.register_event<HealEvent>(ChannelConfig{.delivery = Delivery::Scheduled, .trace = true});
        auto damage = bus.writer<DamageEvent>();
        auto damage_in = bus.reader<DamageEvent>();
        auto heal = bus.writer<HealEvent>();
        auto heal_in = bus.reader<HealEvent>();
        damage.emit({1, 1.0f});
        bus.advance_tick();
        const EventRef root = damage_in.ref(0);

        auto lanes = damage.lanes(2);
        lanes.emit(1, {2, 1.0f}, root);
        heal.emit_after({9, 1.0f}, 2, root);
        bus.advance_tick();
        CHECK(damage_in.cause(0) == root);
        bus.advance_tick();
        REQUIRE(heal_in.size() == 1);
        CHECK(heal_in.cause(0) == root);
        CHECK(bus.effects(root).size() == 2);
    }

    TEST_CASE("untraced channels cost nothing and report no causes; the journal is a ring") {
        EventBus bus;
        bus.register_event<DamageEvent>();
        auto out = bus.writer<DamageEvent>();
        auto in = bus.reader<DamageEvent>();
        out.emit({1, 0.0f}, EventRef::make(0, 0, 0));
        bus.advance_tick();
        CHECK_FALSE(in.cause(0).valid());
        CHECK(bus.trace_journal().empty());

        bus.register_event<HealEvent>(ChannelConfig{.trace = true});
        bus.set_trace_capacity(4);
        auto heal = bus.writer<HealEvent>();
        for (std::uint32_t i = 0; i < 10; ++i) heal.emit({i, 0.0f});
        bus.advance_tick();
        const auto journal = bus.trace_journal();
        REQUIRE(journal.size() == 4);
        CHECK(journal.back().ref.index() == 9); // последние вытесняют старые
        CHECK(journal.front().ref.index() == 6);
    }
}
