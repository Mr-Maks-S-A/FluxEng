#include <EventSystem/Core/Schema.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <unordered_set>

namespace EventSystem {

std::string_view to_string(FieldKind kind) noexcept {
    switch (kind) {
        case FieldKind::Bool: return "bool";
        case FieldKind::Int8: return "i8";
        case FieldKind::Int16: return "i16";
        case FieldKind::Int32: return "i32";
        case FieldKind::Int64: return "i64";
        case FieldKind::UInt8: return "u8";
        case FieldKind::UInt16: return "u16";
        case FieldKind::UInt32: return "u32";
        case FieldKind::UInt64: return "u64";
        case FieldKind::Float32: return "f32";
        case FieldKind::Float64: return "f64";
        case FieldKind::Opaque: return "opaque";
    }
    return "unknown";
}

std::string_view to_string(Layout layout) noexcept {
    switch (layout) {
        case Layout::AoS: return "AoS";
        case Layout::SoA: return "SoA";
    }
    return "unknown";
}

const FieldDesc* EventSchema::find_field(std::string_view field_name) const noexcept {
    const auto index = field_index(field_name);
    return index ? &fields[*index] : nullptr;
}

std::optional<std::size_t> EventSchema::field_index(std::string_view field_name) const noexcept {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == field_name) {
            return i;
        }
    }
    return std::nullopt;
}

namespace {

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

std::optional<std::string> validate_schema(const EventSchema& schema) {
    if (schema.name.empty()) {
        return "event name is empty";
    }
    if (schema.id != make_event_id(schema.name)) {
        return std::format("event '{}': id does not match hash of the name", schema.name);
    }
    if (!std::has_single_bit(schema.alignment)) {
        return std::format("event '{}': alignment {} is not a power of two", schema.name, schema.alignment);
    }
    if (schema.size == 0 || schema.size % schema.alignment != 0) {
        return std::format("event '{}': size {} is not a positive multiple of alignment {}",
                           schema.name, schema.size, schema.alignment);
    }

    std::unordered_set<std::string_view> names;
    for (const FieldDesc& field : schema.fields) {
        if (field.name.empty()) {
            return std::format("event '{}': field with empty name", schema.name);
        }
        if (!names.insert(field.name).second) {
            return std::format("event '{}': duplicate field '{}'", schema.name, field.name);
        }
        if (!std::has_single_bit(field.alignment) || field.alignment > schema.alignment) {
            return std::format("event '{}': field '{}' has invalid alignment {}", schema.name, field.name,
                               field.alignment);
        }
        if (field.size == 0 || field.size % field.alignment != 0) {
            return std::format("event '{}': field '{}' size {} is not a positive multiple of alignment {}",
                               schema.name, field.name, field.size, field.alignment);
        }
        if (field.offset % field.alignment != 0) {
            return std::format("event '{}': field '{}' is misaligned", schema.name, field.name);
        }
        if (field.offset > schema.size || field.size > schema.size - field.offset) {
            return std::format("event '{}': field '{}' is out of event bounds", schema.name, field.name);
        }
    }

    // Пустое событие: C++ даёт ему размер 1, полей нет.
    if (schema.fields.empty()) {
        if (schema.size == 1) {
            return std::nullopt;
        }
        return std::format("event '{}': has {} bytes but no fields", schema.name, schema.size);
    }

    // Поля в порядке смещения должны идти вплотную, с разрывами только на выравнивание.
    std::vector<const FieldDesc*> sorted;
    sorted.reserve(schema.fields.size());
    for (const FieldDesc& field : schema.fields) {
        sorted.push_back(&field);
    }
    std::ranges::sort(sorted, {}, &FieldDesc::offset);

    std::size_t position = 0;
    for (const FieldDesc* field : sorted) {
        if (field->offset < position) {
            return std::format("event '{}': field '{}' overlaps a previous field", schema.name, field->name);
        }
        if (field->offset != align_up(position, field->alignment)) {
            return std::format("event '{}': bytes [{}, {}) are not covered by any field "
                               "(was a member left out of `fields`?)",
                               schema.name, position, field->offset);
        }
        position = field->offset + field->size;
    }
    if (align_up(position, schema.alignment) != schema.size) {
        return std::format("event '{}': bytes [{}, {}) are not covered by any field "
                           "(was a member left out of `fields`?)",
                           schema.name, position, schema.size);
    }
    return std::nullopt;
}

} // namespace EventSystem
