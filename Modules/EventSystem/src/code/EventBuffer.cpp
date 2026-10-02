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
    m_causes.swap(other.m_causes);
    std::swap(m_tracing, other.m_tracing);
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
    note_push();
}

void EventBuffer::append(const EventBuffer& source, std::size_t first, std::size_t count) {
    assert(m_schema == source.m_schema || *m_schema == *source.m_schema);
    assert(first + count <= source.m_size);
    if (count == 0) return;
    if (m_size + count > m_capacity) {
        std::size_t capacity = m_capacity == 0 ? initial_capacity : m_capacity;
        while (capacity < m_size + count) capacity *= 2;
        reserve(capacity);
    }
    for (std::size_t c = 0; c < m_columns.size(); ++c) {
        const std::size_t stride = m_columns[c].stride();
        std::memcpy(m_columns[c].at(m_size), source.m_columns[c].at(first), stride * count);
    }
    m_size += count;
    if (m_tracing) {
        if (source.m_tracing) {
            m_causes.insert(m_causes.end(), source.m_causes.begin() + static_cast<std::ptrdiff_t>(first),
                            source.m_causes.begin() + static_cast<std::ptrdiff_t>(first + count));
        } else {
            m_causes.resize(m_causes.size() + count, 0);
        }
    }
}

void EventBuffer::overwrite(std::size_t index, const EventBuffer& source, std::size_t source_index) noexcept {
    assert(index < m_size && source_index < source.m_size);
    for (std::size_t c = 0; c < m_columns.size(); ++c) {
        std::memcpy(m_columns[c].at(index), source.m_columns[c].at(source_index), m_columns[c].stride());
    }
    if (m_tracing) m_causes[index] = source.m_tracing ? source.m_causes[source_index] : 0;
}

void EventBuffer::erase_front(std::size_t count) noexcept {
    if (count == 0) return;
    if (count >= m_size) {
        clear();
        return;
    }
    const std::size_t rest = m_size - count;
    for (ColumnBuffer& column : m_columns) {
        std::memmove(column.at(0), column.at(count), column.stride() * rest);
    }
    m_size = rest;
    if (m_tracing) m_causes.erase(m_causes.begin(), m_causes.begin() + static_cast<std::ptrdiff_t>(count));
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
