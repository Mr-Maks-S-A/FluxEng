/**
 * @file test_core.cpp
 * @brief Тесты ядра: FNV-1a, идентификаторы, описание событий (compile-time).
 */

#include "TestEvents.hpp"

#include <doctest/doctest.h>

#include <string>
#include <type_traits>
#include <unordered_set>

using namespace EventSystem;
using namespace TestEvents;

// =============================================================================
// Compile-time контракты: если они нарушены, тесты просто не соберутся.
// =============================================================================

static_assert(Event<CollisionEvent>);
static_assert(Event<VoxelChangedEvent>);
static_assert(Event<TickSignalEvent>);

// Намеренно некорректные «события». Вне анонимного namespace, чтобы Clang
// не предупреждал о неиспользуемых статических членах.
struct NotTrivial {
    std::string text;
    static constexpr std::string_view event_name = "test.not_trivial";
    using fields = Fields<Field<"text", &NotTrivial::text>>;
};
struct NoName {
    int value = 0;
    using fields = Fields<Field<"value", &NoName::value>>;
};
struct ForeignField {
    int value = 0;
    static constexpr std::string_view event_name = "test.foreign";
    using fields = Fields<Field<"target", &DamageEvent::target>>;
};

static_assert(!Event<NotTrivial>, "non trivially copyable types are rejected");
static_assert(!Event<NoName>, "event_name is required");
static_assert(!Event<ForeignField>, "fields must belong to the event itself");
static_assert(!Event<int>);

static_assert(event_layout_v<CollisionEvent> == Layout::AoS, "AoS is the default layout");
static_assert(event_layout_v<VoxelChangedEvent> == Layout::SoA);
static_assert(event_field_count_v<CollisionEvent> == 3);
static_assert(event_field_count_v<TickSignalEvent> == 0);
static_assert(field_index_v<CollisionEvent, &CollisionEvent::entity_b> == 1);
static_assert(field_index_v<CollisionEvent, &CollisionEvent::normal> == 2);
static_assert(is_field_of_v<CollisionEvent, &CollisionEvent::normal>);
static_assert(!is_field_of_v<CollisionEvent, &DamageEvent::target>);
static_assert(!is_field_of_v<DamageEvent, &HealEvent::target>, "same shape, different owner");
static_assert(std::is_same_v<member_value_t<&CollisionEvent::normal>, Vec2>);
static_assert(event_field_t<CollisionEvent, 0>::name == "entity_a");

static_assert(event_id_v<DamageEvent> != event_id_v<HealEvent>, "same shape events get different ids");
static_assert(event_id_v<DamageEvent> == event_id_v<DamageEventV2>, "id depends only on the name");

// =============================================================================

TEST_SUITE("Core::FNV1a") {
    TEST_CASE("golden values of FNV-1a 64") {
        // Эталонные значения фиксируют контракт: при неизменном имени ID одинаков
        // на любом компиляторе и платформе.
        CHECK(FNV1a::hash("") == 0xcbf29ce484222325ULL);
        CHECK(FNV1a::hash("a") == 0xaf63dc4c8601ec8cULL);
        CHECK(FNV1a::hash("DummyEventA") == 0x55ca0c6363781d98ULL);
        CHECK(FNV1a::hash("EventBus") == 0x76cbc19a4baad9dfULL);
    }

    TEST_CASE("hash is available at compile time and at runtime") {
        constexpr auto compile_time = FNV1a::hash("physics.collision");
        const std::string runtime_name = "physics.collision";
        CHECK(FNV1a::hash(runtime_name) == compile_time);
    }

    TEST_CASE("hash distinguishes case and content") {
        CHECK(FNV1a::hash("EventA") != FNV1a::hash("EventB"));
        CHECK(FNV1a::hash("EventA") != FNV1a::hash("eventA"));
    }
}

TEST_SUITE("Core::Ids") {
    TEST_CASE("EventId is derived from the event name") {
        CHECK(event_id_v<CollisionEvent> == make_event_id("test.collision"));
        CHECK(event_id_v<CollisionEvent>.value == FNV1a::hash("test.collision"));
    }

    TEST_CASE("EventId and ModuleId work in hash containers") {
        std::unordered_set<EventId> events{event_id_v<DamageEvent>, event_id_v<HealEvent>, event_id_v<DamageEvent>};
        CHECK(events.size() == 2);

        std::unordered_set<ModuleId> modules{ModuleId{0}, ModuleId{1}, ModuleId{0}};
        CHECK(modules.size() == 2);
    }

    TEST_CASE("default ModuleId is invalid") {
        CHECK_FALSE(ModuleId{}.valid());
        CHECK(ModuleId{0}.valid());
    }
}

TEST_SUITE("Core::FixedString") {
    TEST_CASE("view excludes the terminating zero") {
        constexpr FixedString name("normal");
        static_assert(name.view() == "normal");
        CHECK(name.view().size() == 6);
    }
}
