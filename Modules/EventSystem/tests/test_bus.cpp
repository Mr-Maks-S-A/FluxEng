/**
 * @file test_bus.cpp
 * @brief Тесты EventBus: регистрация, конфликты, доступ, тики, рантайм-схемы.
 */

#include "TestEvents.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstring>

using namespace EventSystem;
using namespace TestEvents;

TEST_SUITE("Bus::EventBus") {
    TEST_CASE("register and find channels") {
        EventBus bus;
        CHECK_FALSE(bus.contains<CollisionEvent>());
        CHECK(bus.find(event_id_v<CollisionEvent>) == nullptr);

        IChannel& channel = bus.register_event<CollisionEvent>(ChannelConfig{.reserve = 32});
        CHECK(bus.contains<CollisionEvent>());
        CHECK(bus.find(event_id_v<CollisionEvent>) == &channel);
        CHECK(bus.find("test.collision") == &channel);
        CHECK(bus.find("test.unknown") == nullptr);
        CHECK(channel.name() == "test.collision");
        CHECK(channel.readable().capacity() >= 32);
        CHECK(bus.channel_count() == 1);
        CHECK(&bus.channel_at(0) == &channel);
    }

    TEST_CASE("repeated registration returns the same channel") {
        EventBus bus;
        IChannel& first = bus.register_event<DamageEvent>();
        IChannel& second = bus.register_event<DamageEvent>();
        CHECK(&first == &second);
        CHECK(bus.channel_count() == 1);
    }

    TEST_CASE("repeated registration with config reconfigures the channel") {
        EventBus bus;
        bus.register_event<DamageEvent>();
        IChannel& channel = bus.register_event<DamageEvent>(ChannelConfig{.max_events_per_tick = 5});
        CHECK(channel.config().max_events_per_tick == 5);

        // Без config существующие настройки не меняются.
        bus.register_event<DamageEvent>();
        CHECK(channel.config().max_events_per_tick == 5);
    }

    TEST_CASE("events of the same shape get separate channels") {
        EventBus bus;
        bus.register_event<DamageEvent>();
        bus.register_event<HealEvent>();
        CHECK(bus.channel_count() == 2);

        auto damage = bus.writer<DamageEvent>();
        auto heal_in = bus.reader<HealEvent>();
        damage.emit(DamageEvent{.target = 1, .amount = 5.0f});
        bus.advance_tick();
        CHECK(heal_in.empty());
    }

    TEST_CASE("same name with a different schema is rejected") {
        EventBus bus;
        bus.register_event<DamageEvent>();
        CHECK_THROWS_AS(bus.register_event<DamageEventV2>(), EventSystemError);
    }

    TEST_CASE("invalid schema is rejected") {
        EventBus bus;
        CHECK_THROWS_WITH_AS(bus.register_event<MissingFieldEvent>(),
                             doctest::Contains("not covered"), EventSystemError);
        CHECK(bus.channel_count() == 0);
    }

    TEST_CASE("re-registration with the same delivery policy is accepted") {
        // Пока существует только Stream; отказ при смене политики проверяется,
        // когда появится вторая политика доставки.
        EventBus bus;
        bus.register_event<DamageEvent>();
        CHECK_NOTHROW(bus.register_event<DamageEvent>(ChannelConfig{.delivery = Delivery::Stream}));
    }

    TEST_CASE("writer/reader for unregistered events throw") {
        EventBus bus;
        CHECK_THROWS_WITH_AS((void)bus.writer<DamageEvent>(), doctest::Contains("not registered"), EventSystemError);
        CHECK_THROWS_AS((void)bus.reader<DamageEvent>(), EventSystemError);
    }

    TEST_CASE("typed access to a channel registered with another schema throws") {
        EventBus bus;
        bus.register_event<DamageEvent>();
        CHECK_THROWS_WITH_AS((void)bus.writer<DamageEventV2>(), doctest::Contains("does not match"),
                             EventSystemError);
    }

    TEST_CASE("full tick cycle through the bus") {
        EventBus bus;
        bus.register_event<CollisionEvent>();
        bus.register_event<VoxelChangedEvent>();

        auto collisions_out = bus.writer<CollisionEvent>();
        auto collisions_in = bus.reader<CollisionEvent>();
        auto voxels_out = bus.writer<VoxelChangedEvent>();
        auto voxels_in = bus.reader<VoxelChangedEvent>();

        CHECK(bus.current_tick() == 0);
        collisions_out.emit(CollisionEvent{.entity_a = 1, .entity_b = 2, .normal = {}});
        voxels_out.emit(VoxelChangedEvent{.x = 5, .y = 6, .z = 7, .block = 8});
        bus.advance_tick();

        CHECK(bus.current_tick() == 1);
        REQUIRE(collisions_in.size() == 1);
        CHECK(collisions_in.events()[0].entity_b == 2);
        REQUIRE(voxels_in.size() == 1);
        CHECK(voxels_in.column<&VoxelChangedEvent::block>()[0] == 8);

        bus.advance_tick();
        CHECK(collisions_in.empty());
        CHECK(voxels_in.empty());
    }

    TEST_CASE("clear_all drops events in every channel") {
        EventBus bus;
        bus.register_event<DamageEvent>();
        bus.register_event<HealEvent>();
        bus.writer<DamageEvent>().emit(DamageEvent{});
        bus.writer<HealEvent>().emit(HealEvent{});
        bus.advance_tick();
        bus.writer<DamageEvent>().emit(DamageEvent{});

        bus.clear_all();
        for (std::size_t i = 0; i < bus.channel_count(); ++i) {
            CHECK(bus.channel_at(i).stats().pending == 0);
            CHECK(bus.channel_at(i).stats().readable == 0);
        }
    }

    TEST_CASE("runtime schema: register, emit raw, read raw") {
        // Схема «из скрипта»: C++-типа нет, только описание.
        const EventSchema schema{
            .name = "script.spell_cast",
            .id = make_event_id("script.spell_cast"),
            .size = 8,
            .alignment = 4,
            .layout = Layout::SoA,
            .fields = {
                FieldDesc{.name = "caster", .kind = FieldKind::UInt32, .offset = 0, .size = 4, .alignment = 4},
                FieldDesc{.name = "power", .kind = FieldKind::Float32, .offset = 4, .size = 4, .alignment = 4},
            },
        };

        EventBus bus;
        IChannel& channel = bus.register_schema(schema);

        std::array<std::byte, 8> event{};
        const std::uint32_t caster = 42;
        const float power = 3.5f;
        std::memcpy(event.data() + 0, &caster, 4);
        std::memcpy(event.data() + 4, &power, 4);
        CHECK(channel.emit_raw(event.data()));

        bus.advance_tick();
        const EventBuffer& readable = bus.find("script.spell_cast")->readable();
        REQUIRE(readable.size() == 1);

        const std::size_t power_index = *schema.field_index("power");
        float read_power = 0.0f;
        std::memcpy(&read_power, readable.field_data(0, power_index), sizeof(float));
        CHECK(read_power == 3.5f);
    }

    TEST_CASE("bus is movable; writers stay valid") {
        EventBus bus;
        bus.register_event<DamageEvent>();
        auto writer = bus.writer<DamageEvent>();

        EventBus moved = std::move(bus);
        auto reader = moved.reader<DamageEvent>();
        writer.emit(DamageEvent{.target = 4, .amount = 1.0f});
        moved.advance_tick();
        REQUIRE(reader.size() == 1);
        CHECK(reader.events()[0].target == 4);
    }
}
