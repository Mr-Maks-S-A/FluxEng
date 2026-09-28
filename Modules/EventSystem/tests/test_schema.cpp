/**
 * @file test_schema.cpp
 * @brief Тесты рантайм-схемы: построение из C++-типа и валидация.
 */

#include "TestEvents.hpp"

#include <doctest/doctest.h>

#include <cstddef>

using namespace EventSystem;
using namespace TestEvents;

namespace {

/// Корректная рантайм-схема «как из скрипта»: {u32 id; f32 power;}.
EventSchema make_script_schema() {
    return EventSchema{
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
}

bool rejected(const EventSchema& schema) {
    return validate_schema(schema).has_value();
}

} // namespace

TEST_SUITE("Core::Schema") {
    TEST_CASE("schema_of describes the C++ type") {
        const EventSchema& schema = schema_of<CollisionEvent>();

        CHECK(schema.name == "test.collision");
        CHECK(schema.id == event_id_v<CollisionEvent>);
        CHECK(schema.size == sizeof(CollisionEvent));
        CHECK(schema.alignment == alignof(CollisionEvent));
        CHECK(schema.layout == Layout::AoS);
        REQUIRE(schema.fields.size() == 3);

        CHECK(schema.fields[0].name == "entity_a");
        CHECK(schema.fields[0].kind == FieldKind::UInt32);
        CHECK(schema.fields[0].offset == offsetof(CollisionEvent, entity_a));

        CHECK(schema.fields[2].name == "normal");
        CHECK(schema.fields[2].kind == FieldKind::Opaque);
        CHECK(schema.fields[2].offset == offsetof(CollisionEvent, normal));
        CHECK(schema.fields[2].size == sizeof(Vec2));
    }

    TEST_CASE("schema_of returns the same object every time") {
        CHECK(&schema_of<CollisionEvent>() == &schema_of<CollisionEvent>());
    }

    TEST_CASE("field kinds of scalar types") {
        static_assert(field_kind_of<bool>() == FieldKind::Bool);
        static_assert(field_kind_of<std::int8_t>() == FieldKind::Int8);
        static_assert(field_kind_of<std::int64_t>() == FieldKind::Int64);
        static_assert(field_kind_of<std::uint16_t>() == FieldKind::UInt16);
        static_assert(field_kind_of<const std::uint64_t>() == FieldKind::UInt64);
        static_assert(field_kind_of<float>() == FieldKind::Float32);
        static_assert(field_kind_of<double>() == FieldKind::Float64);
        static_assert(field_kind_of<Vec2>() == FieldKind::Opaque);
        CHECK(to_string(FieldKind::UInt32) == "u32");
        CHECK(to_string(Layout::SoA) == "SoA");
    }

    TEST_CASE("find_field and field_index") {
        const EventSchema& schema = schema_of<VoxelChangedEvent>();
        REQUIRE(schema.find_field("block") != nullptr);
        CHECK(schema.find_field("block")->kind == FieldKind::UInt16);
        CHECK(schema.field_index("z") == 2);
        CHECK(schema.find_field("missing") == nullptr);
        CHECK_FALSE(schema.field_index("missing").has_value());
    }

    TEST_CASE("valid schemas pass validation") {
        CHECK_FALSE(validate_schema(schema_of<CollisionEvent>()).has_value());
        CHECK_FALSE(validate_schema(schema_of<VoxelChangedEvent>()).has_value()); // trailing padding
        CHECK_FALSE(validate_schema(schema_of<TickSignalEvent>()).has_value());   // empty event
        CHECK_FALSE(validate_schema(schema_of<AlignedEvent>()).has_value());      // over-aligned
        CHECK_FALSE(validate_schema(make_script_schema()).has_value());
    }

    TEST_CASE("forgotten member is detected") {
        CHECK(rejected(schema_of<MissingFieldEvent>()));
    }

    TEST_CASE("invalid runtime schemas are rejected") {
        EventSchema schema = make_script_schema();

        SUBCASE("empty name") {
            schema.name.clear();
            schema.id = make_event_id("");
            CHECK(rejected(schema));
        }
        SUBCASE("id does not match name") {
            schema.id = make_event_id("other");
            CHECK(rejected(schema));
        }
        SUBCASE("alignment is not a power of two") {
            schema.alignment = 3;
            CHECK(rejected(schema));
        }
        SUBCASE("size is not a multiple of alignment") {
            schema.size = 10;
            CHECK(rejected(schema));
        }
        SUBCASE("duplicate field names") {
            schema.fields[1].name = "caster";
            CHECK(rejected(schema));
        }
        SUBCASE("empty field name") {
            schema.fields[0].name.clear();
            CHECK(rejected(schema));
        }
        SUBCASE("overlapping fields") {
            schema.fields[1].offset = 2;
            schema.fields[1].alignment = 2;
            schema.fields[1].size = 2;
            CHECK(rejected(schema));
        }
        SUBCASE("misaligned field") {
            schema.fields[1].offset = 5;
            CHECK(rejected(schema));
        }
        SUBCASE("field out of bounds") {
            schema.fields[1].offset = 8;
            CHECK(rejected(schema));
        }
        SUBCASE("uncovered bytes at the end") {
            schema.size = 12;
            CHECK(rejected(schema));
        }
        SUBCASE("non-empty event without fields") {
            schema.fields.clear();
            CHECK(rejected(schema));
        }
    }
}
