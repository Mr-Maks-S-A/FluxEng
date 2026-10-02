/**
 * @file test_policies.cpp
 * @brief Политики доставки (Stream, Coalesced, Scheduled) и домены времени (Tick, Frame).
 *
 * Главное свойство: писатели и читатели одни и те же для всех политик — меняется только то,
 * что видно после смены момента.
 */

#include "TestEvents.hpp"

#include <EventSystem/EventSystem.hpp>

#include <doctest/doctest.h>

#include <vector>

using namespace EventSystem;
using namespace TestEvents;

namespace {

std::vector<DamageEvent> visible(const EventReader<DamageEvent>& reader) {
    return {reader.events().begin(), reader.events().end()};
}

} // namespace

TEST_SUITE("Delivery::Coalesced") {
    TEST_CASE("several events with one key per tick become one: the last value at the place of the first") {
        EventBus bus;
        bus.register_event<DamageEvent>(ChannelConfig{.delivery = Delivery::Coalesced,
                                                      .coalesce_field = coalesce_key<DamageEvent, &DamageEvent::target>()});
        auto out = bus.writer<DamageEvent>();
        auto in = bus.reader<DamageEvent>();
        out.emit({7, 1.0f});
        out.emit({3, 5.0f});
        out.emit({7, 2.0f}); // перекрывает первое: та же цель
        out.emit({7, 9.0f});
        bus.advance_tick();
        const auto events = visible(in);
        REQUIRE(events.size() == 2);
        CHECK(events[0].target == 7);
        CHECK(events[0].amount == 9.0f);
        CHECK(events[1].target == 3);
        CHECK(bus.find("test.damage")->stats().total_coalesced == 2);

        bus.advance_tick(); // слияние только в пределах момента
        CHECK(in.empty());
    }

    TEST_CASE("SoA events coalesce too: columns stay columns") {
        EventBus bus;
        bus.register_event<VoxelChangedEvent>(
            ChannelConfig{.delivery = Delivery::Coalesced, .coalesce_field = coalesce_key<VoxelChangedEvent, &VoxelChangedEvent::x>()});
        auto out = bus.writer<VoxelChangedEvent>();
        auto in = bus.reader<VoxelChangedEvent>();
        for (std::uint16_t i = 0; i < 100; ++i) out.emit({static_cast<std::int32_t>(i % 10), 0, 0, i});
        bus.advance_tick();
        REQUIRE(in.size() == 10);
        const auto blocks = in.column<&VoxelChangedEvent::block>();
        CHECK(blocks[0] == 90); // для x = 0 последним был блок 90
        CHECK(blocks[9] == 99);
    }

    TEST_CASE("a key field longer than 8 bytes is rejected") {
        EventBus bus;
        CHECK_THROWS_AS(bus.register_event<CollisionEvent>(
                            ChannelConfig{.delivery = Delivery::Coalesced, .coalesce_field = 7}),
                        EventSystemError);
    }
}

TEST_SUITE("Delivery::Scheduled") {
    TEST_CASE("emit_after(N) becomes visible exactly N ticks later; emit is emit_after(1)") {
        EventBus bus;
        bus.register_event<DamageEvent>(ChannelConfig{.delivery = Delivery::Scheduled});
        auto out = bus.writer<DamageEvent>();
        auto in = bus.reader<DamageEvent>();
        out.emit_after({3, 0.0f}, 3);
        out.emit_after({1, 0.0f}, 1);
        out.emit({10, 0.0f});
        out.emit_after({2, 0.0f}, 2);
        CHECK(bus.find("test.damage")->stats().scheduled == 3);

        std::vector<std::vector<std::uint32_t>> seen;
        for (int t = 0; t < 4; ++t) {
            bus.advance_tick();
            std::vector<std::uint32_t> now;
            for (const DamageEvent& e : in.events()) now.push_back(e.target);
            seen.push_back(now);
        }
        CHECK(seen[0] == std::vector<std::uint32_t>{1, 10}); // отложенное на 1 — раньше обычного (порядок: старые, затем новые)
        CHECK(seen[1] == std::vector<std::uint32_t>{2});
        CHECK(seen[2] == std::vector<std::uint32_t>{3});
        CHECK(seen[3].empty());
        CHECK(bus.find("test.damage")->stats().scheduled == 0);
    }

    TEST_CASE("events scheduled from a reaction keep their own delay (a regrowing tree)") {
        EventBus bus;
        bus.register_event<DamageEvent>(ChannelConfig{.delivery = Delivery::Scheduled});
        auto out = bus.writer<DamageEvent>();
        auto in = bus.reader<DamageEvent>();
        out.emit_after({1, 0.0f}, 5);
        int regrowths = 0;
        for (int t = 0; t < 30; ++t) {
            bus.advance_tick();
            for (const DamageEvent& e : in.events()) {
                ++regrowths;
                out.emit_after(e, 5); // дерево снова вырастет через 5 тиков
            }
        }
        CHECK(regrowths == 6); // тики 5, 10, 15, 20, 25, 30
    }
}

TEST_SUITE("Domain::Frame") {
    TEST_CASE("frame channels move only on advance_frame, tick channels only on advance_tick") {
        EventBus bus;
        bus.register_event<DamageEvent>(ChannelConfig{.domain = Domain::Frame});
        bus.register_event<HealEvent>(); // Tick
        auto frame_out = bus.writer<DamageEvent>();
        auto frame_in = bus.reader<DamageEvent>();
        auto tick_out = bus.writer<HealEvent>();
        auto tick_in = bus.reader<HealEvent>();
        frame_out.emit({1, 0.0f});
        tick_out.emit({2, 0.0f});

        bus.advance_frame(); // пауза: тиков нет, кадры идут — интерфейс живёт
        CHECK(frame_in.size() == 1);
        CHECK(tick_in.empty());
        bus.advance_frame();
        CHECK(frame_in.empty());
        CHECK(tick_in.empty());
        bus.advance_tick();
        CHECK(tick_in.size() == 1);
        CHECK(bus.current_frame() == 2);
        CHECK(bus.current_tick() == 1);
    }

    TEST_CASE("policy and domain cannot be changed after creation") {
        EventBus bus;
        bus.register_event<DamageEvent>(ChannelConfig{.domain = Domain::Frame});
        CHECK_THROWS_AS(bus.register_event<DamageEvent>(ChannelConfig{.domain = Domain::Tick}), EventSystemError);
        CHECK_THROWS_AS(bus.register_event<DamageEvent>(ChannelConfig{.delivery = Delivery::Scheduled, .domain = Domain::Frame}),
                        EventSystemError);
        CHECK(to_string(Domain::Frame) == "Frame");
        CHECK(to_string(Delivery::Coalesced) == "Coalesced");
    }
}
