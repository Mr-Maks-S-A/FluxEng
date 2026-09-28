/**
 * @file test_storage.cpp
 * @brief Тесты хранилища: ColumnBuffer и EventBuffer в раскладках AoS и SoA.
 */

#include "TestEvents.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace EventSystem;
using namespace TestEvents;

TEST_SUITE("Storage::ColumnBuffer") {
    TEST_CASE("starts empty and allocates on reallocate") {
        ColumnBuffer column(sizeof(std::uint32_t), alignof(std::uint32_t));
        CHECK(column.capacity() == 0);
        CHECK(column.data() == nullptr);
        CHECK(column.allocated_bytes() == 0);

        column.reallocate(10, 0);
        CHECK(column.capacity() == 10);
        CHECK(column.allocated_bytes() == 10 * sizeof(std::uint32_t));
    }

    TEST_CASE("reallocate preserves existing elements") {
        ColumnBuffer column(sizeof(std::uint32_t), alignof(std::uint32_t));
        column.reallocate(4, 0);
        for (std::uint32_t i = 0; i < 4; ++i) {
            std::memcpy(column.at(i), &i, sizeof(i));
        }
        column.reallocate(100, 4);
        for (std::uint32_t i = 0; i < 4; ++i) {
            std::uint32_t value = 0;
            std::memcpy(&value, column.at(i), sizeof(value));
            CHECK(value == i);
        }
    }

    TEST_CASE("over-aligned elements are aligned") {
        ColumnBuffer column(64, 64);
        column.reallocate(3, 0);
        CHECK(reinterpret_cast<std::uintptr_t>(column.data()) % 64 == 0);
    }

    TEST_CASE("move transfers ownership") {
        ColumnBuffer a(4, 4);
        a.reallocate(8, 0);
        const std::byte* data = a.data();

        ColumnBuffer b(std::move(a));
        CHECK(b.data() == data);
        CHECK(b.capacity() == 8);
        CHECK(a.data() == nullptr); // NOLINT(bugprone-use-after-move)

        ColumnBuffer c(4, 4);
        c = std::move(b);
        CHECK(c.data() == data);
    }
}

TEST_SUITE("Storage::EventBuffer") {
    TEST_CASE("AoS buffer has one column with stride == sizeof(event)") {
        EventBuffer buffer(schema_of<CollisionEvent>());
        CHECK(buffer.layout() == Layout::AoS);
        REQUIRE(buffer.column_count() == 1);
        CHECK(buffer.column_buffer(0).stride() == sizeof(CollisionEvent));
    }

    TEST_CASE("SoA buffer has one column per field") {
        EventBuffer buffer(schema_of<VoxelChangedEvent>());
        REQUIRE(buffer.column_count() == 4);
        CHECK(buffer.column_buffer(3).stride() == sizeof(std::uint16_t));
    }

    TEST_CASE("AoS typed push / events / field / get") {
        EventBuffer buffer(schema_of<CollisionEvent>());
        buffer.push(CollisionEvent{.entity_a = 1, .entity_b = 2, .normal = {0.0f, 1.0f}});
        buffer.push(CollisionEvent{.entity_a = 3, .entity_b = 4, .normal = {1.0f, 0.0f}});

        REQUIRE(buffer.size() == 2);
        const auto events = buffer.events<CollisionEvent>();
        CHECK(events[0].entity_b == 2);
        CHECK(events[1].normal == Vec2{1.0f, 0.0f});
        CHECK(buffer.field<CollisionEvent, &CollisionEvent::entity_a>(1) == 3);
        CHECK(buffer.get<CollisionEvent>(0).normal == Vec2{0.0f, 1.0f});
    }

    TEST_CASE("SoA typed push / column / field / get") {
        EventBuffer buffer(schema_of<VoxelChangedEvent>());
        for (int i = 0; i < 5; ++i) {
            buffer.push(VoxelChangedEvent{.x = i, .y = -i, .z = i * 10, .block = static_cast<std::uint16_t>(100 + i)});
        }

        const auto blocks = buffer.column<VoxelChangedEvent, &VoxelChangedEvent::block>();
        const auto zs = buffer.column<VoxelChangedEvent, &VoxelChangedEvent::z>();
        REQUIRE(blocks.size() == 5);
        CHECK(blocks[4] == 104);
        CHECK(zs[3] == 30);
        CHECK(buffer.field<VoxelChangedEvent, &VoxelChangedEvent::y>(2) == -2);

        const VoxelChangedEvent event = buffer.get<VoxelChangedEvent>(1);
        CHECK(event.x == 1);
        CHECK(event.y == -1);
        CHECK(event.z == 10);
        CHECK(event.block == 101);
    }

    TEST_CASE("raw push and read round-trip in both layouts") {
        const CollisionEvent aos_in{.entity_a = 7, .entity_b = 8, .normal = {0.5f, -0.5f}};
        EventBuffer aos(schema_of<CollisionEvent>());
        aos.push_raw(reinterpret_cast<const std::byte*>(&aos_in));
        CollisionEvent aos_out{};
        aos.read_raw(0, reinterpret_cast<std::byte*>(&aos_out));
        CHECK(aos_out.entity_a == 7);
        CHECK(aos_out.normal == aos_in.normal);

        const VoxelChangedEvent soa_in{.x = 1, .y = 2, .z = 3, .block = 4};
        EventBuffer soa(schema_of<VoxelChangedEvent>());
        soa.push_raw(reinterpret_cast<const std::byte*>(&soa_in));
        CHECK(soa.column<VoxelChangedEvent, &VoxelChangedEvent::y>()[0] == 2);
        VoxelChangedEvent soa_out{};
        soa.read_raw(0, reinterpret_cast<std::byte*>(&soa_out));
        CHECK(soa_out.block == 4);
        CHECK(soa_out.z == 3);
    }

    TEST_CASE("field_data gives the same value regardless of layout") {
        EventBuffer aos(schema_of<CollisionEvent>());
        aos.push(CollisionEvent{.entity_a = 11, .entity_b = 22, .normal = {}});
        std::uint32_t value = 0;
        std::memcpy(&value, aos.field_data(0, 1), sizeof(value));
        CHECK(value == 22);

        EventBuffer soa(schema_of<VoxelChangedEvent>());
        soa.push(VoxelChangedEvent{.x = 0, .y = 0, .z = 0, .block = 77});
        std::uint16_t block = 0;
        std::memcpy(&block, soa.field_data(0, 3), sizeof(block));
        CHECK(block == 77);
    }

    TEST_CASE("growth preserves data across many reallocations") {
        EventBuffer buffer(schema_of<VoxelChangedEvent>());
        constexpr int count = 5000;
        for (int i = 0; i < count; ++i) {
            buffer.push(VoxelChangedEvent{.x = i, .y = i + 1, .z = i + 2, .block = static_cast<std::uint16_t>(i)});
        }
        REQUIRE(buffer.size() == count);
        CHECK(buffer.capacity() >= count);

        bool all_valid = true;
        for (int i = 0; i < count; ++i) {
            const auto event = buffer.get<VoxelChangedEvent>(static_cast<std::size_t>(i));
            all_valid = all_valid && event.x == i && event.y == i + 1 && event.z == i + 2 &&
                        event.block == static_cast<std::uint16_t>(i);
        }
        CHECK(all_valid);
    }

    TEST_CASE("randomized AoS vs SoA equivalence") {
        std::mt19937 rng(1337);
        std::uniform_int_distribution<std::int32_t> coord(-1000, 1000);
        std::uniform_int_distribution<int> block(0, 65535);

        EventBuffer soa(schema_of<VoxelChangedEvent>());
        std::vector<VoxelChangedEvent> reference;
        for (int i = 0; i < 2000; ++i) {
            const VoxelChangedEvent event{.x = coord(rng), .y = coord(rng), .z = coord(rng),
                                          .block = static_cast<std::uint16_t>(block(rng))};
            reference.push_back(event);
            soa.push(event);
        }

        const auto xs = soa.column<VoxelChangedEvent, &VoxelChangedEvent::x>();
        const auto blocks = soa.column<VoxelChangedEvent, &VoxelChangedEvent::block>();
        bool all_valid = true;
        for (std::size_t i = 0; i < reference.size(); ++i) {
            all_valid = all_valid && xs[i] == reference[i].x && blocks[i] == reference[i].block;
        }
        CHECK(all_valid);
    }

    TEST_CASE("reserve, clear and allocated_bytes") {
        EventBuffer buffer(schema_of<CollisionEvent>());
        CHECK(buffer.allocated_bytes() == 0);

        buffer.reserve(100);
        CHECK(buffer.capacity() == 100);
        CHECK(buffer.allocated_bytes() == 100 * sizeof(CollisionEvent));

        buffer.reserve(10); // уменьшение не делает ничего
        CHECK(buffer.capacity() == 100);

        buffer.push(CollisionEvent{});
        buffer.clear();
        CHECK(buffer.empty());
        CHECK(buffer.capacity() == 100);
    }

    TEST_CASE("SoA columns start on a cache line; 32-byte lanes stay 32-byte aligned") {
        EventBuffer buffer(schema_of<AlignedEvent>());
        AlignedEvent event{};
        event.lanes[7] = 42.0f;
        event.id = 9;
        buffer.push(event);
        buffer.push(event);

        for (std::size_t c = 0; c < buffer.column_count(); ++c) {
            CHECK(reinterpret_cast<std::uintptr_t>(buffer.column_buffer(c).data()) % ColumnBuffer::base_alignment == 0);
        }
        const auto lanes = buffer.column<AlignedEvent, &AlignedEvent::lanes>();
        CHECK(reinterpret_cast<std::uintptr_t>(&lanes[1]) % 32 == 0);
        CHECK(lanes[1][7] == 42.0f);
        CHECK(buffer.field<AlignedEvent, &AlignedEvent::id>(1) == 9);
    }

    TEST_CASE("empty event only counts") {
        EventBuffer buffer(schema_of<TickSignalEvent>());
        buffer.push(TickSignalEvent{});
        buffer.push(TickSignalEvent{});
        CHECK(buffer.size() == 2);
        CHECK(buffer.events<TickSignalEvent>().size() == 2);
    }

    TEST_CASE("swap exchanges contents without reallocation") {
        const EventSchema& schema = schema_of<DamageEvent>();
        EventBuffer a(schema);
        EventBuffer b(schema);
        a.push(DamageEvent{.target = 1, .amount = 5.0f});
        const auto* data = a.column_buffer(0).data();

        a.swap(b);
        CHECK(a.empty());
        REQUIRE(b.size() == 1);
        CHECK(b.column_buffer(0).data() == data);
        CHECK(b.events<DamageEvent>()[0].target == 1);
    }
}
