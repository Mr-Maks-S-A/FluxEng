/**
 * @file test_channel.cpp
 * @brief Тесты StreamChannel: семантика тиков, бюджет, статистика, настройки.
 */

#include "TestEvents.hpp"

#include <doctest/doctest.h>

#include <cstring>

using namespace EventSystem;
using namespace TestEvents;

TEST_SUITE("Channel::StreamChannel") {
    TEST_CASE("events emitted in tick N are readable only in tick N+1") {
        StreamChannel channel(schema_of<DamageEvent>(), ChannelConfig{});
        EventWriter<DamageEvent> writer(channel);
        EventReader<DamageEvent> reader(channel);

        // Тик 0: отправили, но ещё не видно.
        writer.emit(DamageEvent{.target = 1, .amount = 10.0f});
        CHECK(writer.pending_count() == 1);
        CHECK(reader.empty());

        // Тик 1: видно.
        channel.advance();
        REQUIRE(reader.size() == 1);
        CHECK(reader.events()[0].target == 1);
        CHECK(writer.pending_count() == 0);

        // Тик 2: пропало.
        channel.advance();
        CHECK(reader.empty());
    }

    TEST_CASE("events emitted while reading do not mix with readable ones") {
        StreamChannel channel(schema_of<DamageEvent>(), ChannelConfig{});
        EventWriter<DamageEvent> writer(channel);
        EventReader<DamageEvent> reader(channel);

        writer.emit(DamageEvent{.target = 1, .amount = 1.0f});
        channel.advance();

        // Реакция на событие в том же тике уходит в следующий тик.
        for (const DamageEvent& event : reader.events()) {
            writer.emit(DamageEvent{.target = event.target + 1, .amount = 1.0f});
        }
        CHECK(reader.size() == 1);

        channel.advance();
        REQUIRE(reader.size() == 1);
        CHECK(reader.events()[0].target == 2);
    }

    TEST_CASE("steady state does not allocate") {
        StreamChannel channel(schema_of<DamageEvent>(), ChannelConfig{.reserve = 128});
        EventWriter<DamageEvent> writer(channel);
        const std::size_t bytes = channel.stats().allocated_bytes;

        for (int tick = 0; tick < 10; ++tick) {
            for (int i = 0; i < 100; ++i) {
                writer.emit(DamageEvent{});
            }
            channel.advance();
        }
        CHECK(channel.stats().allocated_bytes == bytes);
    }

    TEST_CASE("budget drops events above max_events_per_tick") {
        StreamChannel channel(schema_of<DamageEvent>(), ChannelConfig{.max_events_per_tick = 3});
        EventWriter<DamageEvent> writer(channel);

        int accepted = 0;
        for (int i = 0; i < 5; ++i) {
            accepted += writer.emit(DamageEvent{}) ? 1 : 0;
        }
        CHECK(accepted == 3);
        CHECK(channel.stats().total_dropped == 2);

        // Бюджет считается на тик.
        channel.advance();
        CHECK(writer.emit(DamageEvent{}));
    }

    TEST_CASE("emit_raw respects the budget too") {
        StreamChannel channel(schema_of<DamageEvent>(), ChannelConfig{.max_events_per_tick = 1});
        const DamageEvent event{.target = 3, .amount = 2.0f};
        const auto* bytes = reinterpret_cast<const std::byte*>(&event);
        CHECK(channel.emit_raw(bytes));
        CHECK_FALSE(channel.emit_raw(bytes));
        CHECK(channel.stats().total_dropped == 1);
    }

    TEST_CASE("stats track emitted, peak and readable counts") {
        StreamChannel channel(schema_of<DamageEvent>(), ChannelConfig{});
        EventWriter<DamageEvent> writer(channel);

        for (int i = 0; i < 4; ++i) writer.emit(DamageEvent{});
        channel.advance();
        for (int i = 0; i < 2; ++i) writer.emit(DamageEvent{});

        const ChannelStats stats = channel.stats();
        CHECK(stats.pending == 2);
        CHECK(stats.readable == 4);
        CHECK(stats.peak_per_tick == 4);
        CHECK(stats.total_emitted == 4); // pending учтутся при следующем advance

        channel.advance();
        CHECK(channel.stats().total_emitted == 6);
    }

    TEST_CASE("clear removes pending and readable events") {
        StreamChannel channel(schema_of<DamageEvent>(), ChannelConfig{});
        EventWriter<DamageEvent> writer(channel);
        writer.emit(DamageEvent{});
        channel.advance();
        writer.emit(DamageEvent{});

        channel.clear();
        CHECK(channel.stats().pending == 0);
        CHECK(channel.stats().readable == 0);
    }

    TEST_CASE("configure changes budget and reserve, not delivery") {
        StreamChannel channel(schema_of<DamageEvent>(), ChannelConfig{});
        channel.configure(ChannelConfig{.reserve = 256, .max_events_per_tick = 1});
        CHECK(channel.config().max_events_per_tick == 1);
        CHECK(channel.readable().capacity() >= 256);
        CHECK(channel.delivery() == Delivery::Stream);
        CHECK(to_string(Delivery::Stream) == "Stream");
    }

    TEST_CASE("readable() exposes the same data as the typed reader") {
        StreamChannel channel(schema_of<VoxelChangedEvent>(), ChannelConfig{});
        EventWriter<VoxelChangedEvent> writer(channel);
        writer.emit(VoxelChangedEvent{.x = 1, .y = 2, .z = 3, .block = 9});
        channel.advance();

        const EventBuffer& raw = channel.readable();
        REQUIRE(raw.size() == 1);
        std::uint16_t block = 0;
        std::memcpy(&block, raw.field_data(0, 3), sizeof(block));
        CHECK(block == 9);
    }

    TEST_CASE("SoA reader: column, field, get, for_each") {
        StreamChannel channel(schema_of<VoxelChangedEvent>(), ChannelConfig{});
        EventWriter<VoxelChangedEvent> writer(channel);
        EventReader<VoxelChangedEvent> reader(channel);
        for (int i = 0; i < 3; ++i) {
            writer.emit(VoxelChangedEvent{.x = i, .y = 0, .z = 0, .block = static_cast<std::uint16_t>(i * 2)});
        }
        channel.advance();

        CHECK(reader.column<&VoxelChangedEvent::block>()[2] == 4);
        CHECK(reader.field<&VoxelChangedEvent::x>(1) == 1);
        CHECK(reader.get(2).x == 2);

        int sum = 0;
        reader.for_each([&](const VoxelChangedEvent& event) { sum += event.x; });
        CHECK(sum == 3);
    }

    TEST_CASE("default-constructed writer and reader are invalid") {
        CHECK_FALSE(EventWriter<DamageEvent>{}.valid());
        CHECK_FALSE(EventReader<DamageEvent>{}.valid());
    }
}
