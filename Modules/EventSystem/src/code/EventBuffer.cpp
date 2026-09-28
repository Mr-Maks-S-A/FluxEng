#include <EventSystem/Storage/EventBuffer.hpp>

namespace EventSystem {

EventBuffer::EventBuffer(const EventSchema& schema) : m_schema(&schema) {
    if (schema.layout == Layout::AoS) {
        m_columns.emplace_back(schema.size, schema.alignment);
    } else {
        m_columns.reserve(schema.fields.size());
        for (const FieldDesc& field : schema.fields) {
            m_columns.emplace_back(field.size, field.alignment);
        }
    }
}

void EventBuffer::swap(EventBuffer& other) noexcept {
    assert(m_schema == other.m_schema && "EventBuffer::swap: buffers must share one schema");
    m_columns.swap(other.m_columns);
    std::swap(m_size, other.m_size);
    std::swap(m_capacity, other.m_capacity);
}

std::size_t EventBuffer::allocated_bytes() const noexcept {
    std::size_t total = 0;
    for (const ColumnBuffer& column : m_columns) {
        total += column.allocated_bytes();
    }
    return total;
}

void EventBuffer::reserve(std::size_t capacity) {
    if (capacity <= m_capacity) {
        return;
    }
    // Если одна из колонок бросит bad_alloc, у уже перевыделенных колонок вместимость
    // станет больше m_capacity — это безопасно: инвариант «каждая колонка >= m_capacity» сохраняется.
    for (ColumnBuffer& column : m_columns) {
        column.reallocate(capacity, m_size);
    }
    m_capacity = capacity;
}

void EventBuffer::grow() {
    reserve(m_capacity == 0 ? initial_capacity : m_capacity * 2);
}

void EventBuffer::push_raw(const std::byte* event) {
    if (m_size == m_capacity) [[unlikely]] {
        grow();
    }
    if (layout() == Layout::AoS) {
        std::memcpy(m_columns[0].at(m_size), event, m_schema->size);
    } else {
        const auto& fields = m_schema->fields;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            std::memcpy(m_columns[i].at(m_size), event + fields[i].offset, fields[i].size);
        }
    }
    ++m_size;
}

void EventBuffer::read_raw(std::size_t index, std::byte* out) const noexcept {
    assert(index < m_size);
    if (layout() == Layout::AoS) {
        std::memcpy(out, m_columns[0].at(index), m_schema->size);
    } else {
        const auto& fields = m_schema->fields;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            std::memcpy(out + fields[i].offset, m_columns[i].at(index), fields[i].size);
        }
    }
}

const std::byte* EventBuffer::field_data(std::size_t index, std::size_t field) const noexcept {
    assert(index < m_size && field < m_schema->fields.size());
    if (layout() == Layout::AoS) {
        return m_columns[0].at(index) + m_schema->fields[field].offset;
    }
    return m_columns[field].at(index);
}

} // namespace EventSystem
