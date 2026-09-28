#pragma once
/**
 * @file TestEvents.hpp
 * @brief События, общие для всех тестов модуля.
 */

#include <EventSystem/EventSystem.hpp>

#include <cstdint>
#include <string_view>

namespace TestEvents {

namespace es = EventSystem;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
    bool operator==(const Vec2&) const = default;
};

/// AoS-событие со вложенной структурой.
struct CollisionEvent {
    std::uint32_t entity_a = 0;
    std::uint32_t entity_b = 0;
    Vec2 normal;

    static constexpr std::string_view event_name = "test.collision";
    using fields = es::Fields<
        es::Field<"entity_a", &CollisionEvent::entity_a>,
        es::Field<"entity_b", &CollisionEvent::entity_b>,
        es::Field<"normal", &CollisionEvent::normal>>;
};

/// SoA-событие с padding в конце (14 байт полей + 2 байта выравнивания).
struct VoxelChangedEvent {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint16_t block = 0;

    static constexpr std::string_view event_name = "test.voxel_changed";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<
        es::Field<"x", &VoxelChangedEvent::x>,
        es::Field<"y", &VoxelChangedEvent::y>,
        es::Field<"z", &VoxelChangedEvent::z>,
        es::Field<"block", &VoxelChangedEvent::block>>;
};

/// Два события одинаковой формы: разные типы, разные каналы.
struct DamageEvent {
    std::uint32_t target = 0;
    float amount = 0.0f;

    static constexpr std::string_view event_name = "test.damage";
    using fields = es::Fields<
        es::Field<"target", &DamageEvent::target>,
        es::Field<"amount", &DamageEvent::amount>>;
};

struct HealEvent {
    std::uint32_t target = 0;
    float amount = 0.0f;

    static constexpr std::string_view event_name = "test.heal";
    using fields = es::Fields<
        es::Field<"target", &HealEvent::target>,
        es::Field<"amount", &HealEvent::amount>>;
};

/// Событие-сигнал без данных.
struct TickSignalEvent {
    static constexpr std::string_view event_name = "test.tick_signal";
    using fields = es::Fields<>;
};

/// SoA-событие с перевыровненным полем: проверка aligned-аллокаций.
struct AlignedEvent {
    alignas(32) float lanes[8] = {};
    std::uint32_t id = 0;

    static constexpr std::string_view event_name = "test.aligned";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<
        es::Field<"lanes", &AlignedEvent::lanes>,
        es::Field<"id", &AlignedEvent::id>>;
};

/// Некорректное событие: член `b` забыли указать в fields.
struct MissingFieldEvent {
    std::uint32_t a = 0;
    std::uint32_t b = 0;

    static constexpr std::string_view event_name = "test.missing_field";
    using fields = es::Fields<es::Field<"a", &MissingFieldEvent::a>>;
};

/// Некорректное событие: то же имя, что у DamageEvent, но другая схема.
struct DamageEventV2 {
    std::uint32_t target = 0;
    double amount = 0.0;

    static constexpr std::string_view event_name = "test.damage";
    using fields = es::Fields<
        es::Field<"target", &DamageEventV2::target>,
        es::Field<"amount", &DamageEventV2::amount>>;
};

} // namespace TestEvents
