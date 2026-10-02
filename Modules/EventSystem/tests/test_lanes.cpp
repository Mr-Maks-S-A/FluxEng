/**
 * @file test_lanes.cpp
 * @brief Дорожки потоков: запись из многих потоков без мьютексов, SoA сохраняется, порядок детерминирован.
 */

#include "TestEvents.hpp"

#include <EventSystem/EventSystem.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

using namespace EventSystem;
using namespace TestEvents;

namespace {

/// Кусок `chunk` пишет события своих элементов [chunk·grain, …) — как кусок JobSystem::parallel_for.
void produce(LaneWriter<VoxelChangedEvent>& lanes, std::size_t chunk, std::size_t grain, std::size_t count) {
    const std::size_t begin = chunk * grain;
    const std::size_t end = std::min(begin + grain, count);
    for (std::size_t i = begin; i < end; ++i) {
        if (i % 3 == 0) {
            lanes.emit(chunk, VoxelChangedEvent{static_cast<std::int32_t>(i), static_cast<std::int32_t>(i * 2), -1,
                                                static_cast<std::uint16_t>(i % 1000)});
        }
    }
}

/// Прогон: `threads` потоков разбирают куски через атомарный счётчик (раздача работы, не запись событий).
std::vector<std::int32_t> run(unsigned threads) {
    EventBus bus;
    bus.register_event<VoxelChangedEvent>();
    auto writer = bus.writer<VoxelChangedEvent>();
    auto reader = bus.reader<VoxelChangedEvent>();
    constexpr std::size_t count = 100'000;
    constexpr std::size_t grain = 1'000;
    const std::size_t chunks = (count + grain - 1) / grain;

    writer.emit(VoxelChangedEvent{-7, 0, 0, 0}); // событие главного потока — идёт первым
    LaneWriter<VoxelChangedEvent> lanes = writer.lanes(chunks);
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (std::size_t chunk = next.fetch_add(1); chunk < chunks; chunk = next.fetch_add(1)) {
                produce(lanes, chunk, grain, count);
            }
        });
    }
    for (std::thread& w : workers) w.join();
    bus.advance_tick();

    const auto xs = reader.column<&VoxelChangedEvent::x>();
    return {xs.begin(), xs.end()};
}

} // namespace

TEST_SUITE("LaneWriter") {
    TEST_CASE("many threads write SoA events without locks; the merged order is the same for 1 and 8 threads") {
        const std::vector<std::int32_t> one = run(1);
        const std::vector<std::int32_t> eight = run(8);
        REQUIRE(one.size() == 1 + 33'334);
        CHECK(one == eight);
        CHECK(one[0] == -7);                       // главный поток — первым
        CHECK(std::ranges::is_sorted(one.begin() + 1, one.end())); // дорожки — по порядку кусков
    }

    TEST_CASE("merged SoA columns are contiguous and every field survives") {
        EventBus bus;
        bus.register_event<VoxelChangedEvent>();
        auto writer = bus.writer<VoxelChangedEvent>();
        auto reader = bus.reader<VoxelChangedEvent>();
        auto lanes = writer.lanes(2);
        lanes.emit(1, {10, 20, 30, 40});
        lanes.emit(0, {1, 2, 3, 4});
        lanes.emit(1, {11, 21, 31, 41});
        CHECK(lanes.lane_size(1) == 2);
        bus.advance_tick();
        REQUIRE(reader.size() == 3);
        CHECK(reader.column<&VoxelChangedEvent::x>()[0] == 1); // дорожка 0 раньше дорожки 1
        CHECK(reader.column<&VoxelChangedEvent::block>()[2] == 41);
        CHECK(reader.get(1).z == 30);
    }

    TEST_CASE("two systems open lanes in one tick: merged in the order of opening") {
        EventBus bus;
        bus.register_event<DamageEvent>();
        auto writer = bus.writer<DamageEvent>();
        auto reader = bus.reader<DamageEvent>();
        auto first = writer.lanes(2);
        auto second = writer.lanes(1);
        second.emit(0, {300, 0.0f});
        first.emit(1, {200, 0.0f});
        first.emit(0, {100, 0.0f});
        bus.advance_tick();
        REQUIRE(reader.size() == 3);
        CHECK(reader.events()[0].target == 100);
        CHECK(reader.events()[1].target == 200);
        CHECK(reader.events()[2].target == 300);
        bus.advance_tick();
        CHECK(reader.empty()); // дорожки очищены при слиянии
    }

    TEST_CASE("the budget is applied at merge time: the tail is dropped, deterministically") {
        EventBus bus;
        bus.register_event<DamageEvent>(ChannelConfig{.max_events_per_tick = 5});
        auto writer = bus.writer<DamageEvent>();
        auto reader = bus.reader<DamageEvent>();
        writer.emit({0, 0.0f});
        auto lanes = writer.lanes(3);
        for (std::uint32_t i = 0; i < 3; ++i) lanes.emit(0, {10 + i, 0.0f});
        for (std::uint32_t i = 0; i < 3; ++i) lanes.emit(1, {20 + i, 0.0f});
        bus.advance_tick();
        REQUIRE(reader.size() == 5);
        CHECK(reader.events()[4].target == 20);
        CHECK(bus.find("test.damage")->stats().total_dropped == 2);
    }

    TEST_CASE("lanes work with every policy: Coalesced merges lane events as well") {
        EventBus bus;
        bus.register_event<DamageEvent>(ChannelConfig{.delivery = Delivery::Coalesced,
                                                      .coalesce_field = coalesce_key<DamageEvent, &DamageEvent::target>()});
        auto writer = bus.writer<DamageEvent>();
        auto reader = bus.reader<DamageEvent>();
        auto lanes = writer.lanes(4);
        for (std::size_t lane = 0; lane < 4; ++lane) lanes.emit(lane, {42, static_cast<float>(lane)});
        bus.advance_tick();
        REQUIRE(reader.size() == 1);
        CHECK(reader.events()[0].amount == 3.0f); // последняя дорожка — последнее значение
    }

    TEST_CASE("lane memory is reused: no growth after warm-up") {
        EventBus bus;
        bus.register_event<DamageEvent>();
        auto writer = bus.writer<DamageEvent>();
        std::size_t bytes = 0;
        for (int tick = 0; tick < 10; ++tick) {
            auto lanes = writer.lanes(8);
            for (std::size_t lane = 0; lane < 8; ++lane)
                for (std::uint32_t i = 0; i < 100; ++i) lanes.emit(lane, {i, 0.0f});
            bus.advance_tick();
            const std::size_t now = bus.find("test.damage")->stats().allocated_bytes;
            if (tick == 2) bytes = now;
            if (tick > 2) CHECK(now == bytes);
        }
        CHECK(bus.find("test.damage")->stats().lanes == 8);
    }
}
