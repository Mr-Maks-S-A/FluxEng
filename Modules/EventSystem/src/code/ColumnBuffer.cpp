#include <EventSystem/Storage/ColumnBuffer.hpp>

#include <cassert>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace EventSystem {

ColumnBuffer::ColumnBuffer(std::size_t stride, std::size_t alignment) noexcept
    : m_stride(stride), m_alignment(alignment) {
    assert(stride > 0 && alignment > 0 && stride % alignment == 0);
}

ColumnBuffer::~ColumnBuffer() {
    release();
}

ColumnBuffer::ColumnBuffer(ColumnBuffer&& other) noexcept
    : m_data(std::exchange(other.m_data, nullptr)),
      m_capacity(std::exchange(other.m_capacity, 0)),
      m_stride(other.m_stride),
      m_alignment(other.m_alignment) {}

ColumnBuffer& ColumnBuffer::operator=(ColumnBuffer&& other) noexcept {
    if (this != &other) {
        release();
        m_data = std::exchange(other.m_data, nullptr);
        m_capacity = std::exchange(other.m_capacity, 0);
        m_stride = other.m_stride;
        m_alignment = other.m_alignment;
    }
    return *this;
}

void ColumnBuffer::reallocate(std::size_t new_capacity, std::size_t preserved_count) {
    assert(preserved_count <= new_capacity && preserved_count <= m_capacity);

    if (new_capacity > std::numeric_limits<std::size_t>::max() / m_stride) {
        throw std::length_error("ColumnBuffer: capacity overflow");
    }

    std::byte* new_data = nullptr;
    if (new_capacity > 0) {
        new_data = static_cast<std::byte*>(::operator new(new_capacity * m_stride, std::align_val_t{allocation_alignment()}));
        if (preserved_count > 0) {
            std::memcpy(new_data, m_data, preserved_count * m_stride);
        }
    }

    release();
    m_data = new_data;
    m_capacity = new_capacity;
}

void ColumnBuffer::release() noexcept {
    if (m_data != nullptr) {
        ::operator delete(m_data, std::align_val_t{allocation_alignment()});
        m_data = nullptr;
    }
    m_capacity = 0;
}

} // namespace EventSystem
