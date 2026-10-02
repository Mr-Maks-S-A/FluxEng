#include <MemorySystem/Arena.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace MemorySystem {

Arena Arena::reserve(std::size_t capacity, std::size_t commit_step, MemoryTag tag) noexcept {
    Arena arena;
    VirtualRegion region = VirtualRegion::reserve(capacity);
    if (!region) {
        return arena;
    }
    arena.m_tag = tag;
    arena.m_tracked = true;
    detail::track_region(tag, +1);
    arena.m_base = region.data();
    arena.m_capacity = region.reserved();
    arena.m_commit_step = align_up(std::max<std::size_t>(commit_step, 1), page_size());
    arena.m_region = std::move(region);
    return arena;
}

Arena Arena::over(std::span<std::byte> buffer) noexcept {
    Arena arena;
    if (buffer.empty()) {
        return arena;
    }
    std::memset(buffer.data(), 0, buffer.size());
    arena.m_base = buffer.data();
    arena.m_capacity = buffer.size();
    arena.m_committed = buffer.size(); // внешний буфер «подтверждён» целиком
    return arena;
}

Arena::Arena(Arena&& other) noexcept
    : m_region(std::move(other.m_region)),
      m_base(std::exchange(other.m_base, nullptr)),
      m_position(std::exchange(other.m_position, 0)),
      m_committed(std::exchange(other.m_committed, 0)),
      m_capacity(std::exchange(other.m_capacity, 0)),
      m_peak(std::exchange(other.m_peak, 0)),
      m_commit_step(std::exchange(other.m_commit_step, 0)),
      m_tag(other.m_tag),
      m_tracked(std::exchange(other.m_tracked, false)) {}

Arena::~Arena() {
    untrack();
}

void Arena::untrack() noexcept {
    if (m_tracked) {
        detail::track_decommit(m_tag, m_committed);
        detail::track_region(m_tag, -1);
        m_tracked = false;
    }
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        untrack();
        m_region = std::move(other.m_region);
        m_base = std::exchange(other.m_base, nullptr);
        m_position = std::exchange(other.m_position, 0);
        m_committed = std::exchange(other.m_committed, 0);
        m_capacity = std::exchange(other.m_capacity, 0);
        m_peak = std::exchange(other.m_peak, 0);
        m_commit_step = std::exchange(other.m_commit_step, 0);
        m_tag = other.m_tag;
        m_tracked = std::exchange(other.m_tracked, false);
    }
    return *this;
}

void* Arena::push(std::size_t size, std::size_t alignment) noexcept {
    assert(is_power_of_two(alignment) && "Arena::push: alignment must be a power of two");
    if (m_base == nullptr) {
        return nullptr;
    }
    const auto base = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(m_base));
    const std::size_t start = align_up(base + m_position, alignment) - base;
    if (start > m_capacity || size > m_capacity - start) {
        return nullptr;
    }
    const std::size_t end = start + size;
    if (end > m_committed && !grow_commit(end)) {
        return nullptr;
    }
    m_position = end;
    m_peak = std::max(m_peak, m_position);
    return m_base + start; // уже нули: инвариант ZII
}

bool Arena::grow_commit(std::size_t required_end) noexcept {
    if (!m_region) {
        return false; // внешний буфер не растёт
    }
    const std::size_t target = std::min(align_up(required_end, m_commit_step), m_capacity);
    if (!m_region.commit(m_committed, target - m_committed)) {
        return false;
    }
    if (m_tracked) detail::track_commit(m_tag, target - m_committed);
    m_committed = target;
    return true;
}

void Arena::pop_to(ArenaMarker marker) noexcept {
    assert(marker.position <= m_position && "Arena::pop_to: marker is ahead of the arena");
    if (marker.position >= m_position) {
        return;
    }
    std::memset(m_base + marker.position, 0, m_position - marker.position); // восстанавливаем инвариант ZII
    m_position = marker.position;
}

void Arena::shrink() noexcept {
    if (!m_region) {
        return;
    }
    const std::size_t keep = align_up(m_position, page_size());
    if (keep >= m_committed) {
        return;
    }
    m_region.decommit(keep, m_committed - keep);
    if (m_tracked) detail::track_decommit(m_tag, m_committed - keep);
    m_committed = keep;
}

} // namespace MemorySystem
