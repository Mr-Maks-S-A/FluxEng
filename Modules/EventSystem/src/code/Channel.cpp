#include <EventSystem/Channel/Channel.hpp>
#include <EventSystem/Core/Error.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

namespace EventSystem {

std::string_view to_string(Delivery delivery) noexcept {
    switch (delivery) {
        case Delivery::Stream: return "Stream";
        case Delivery::Coalesced: return "Coalesced";
        case Delivery::Scheduled: return "Scheduled";
    }
    return "unknown";
}

std::string_view to_string(Domain domain) noexcept {
    return domain == Domain::Frame ? "Frame" : "Tick";
}

Channel::Channel(EventSchema schema, const ChannelConfig& config)
    : m_schema(std::move(schema)),
      m_config(config),
      m_pending(m_schema),
      m_ready(m_schema),
      m_future(m_schema),
      m_kept(m_schema) {
    apply_config();
}

void Channel::configure(const ChannelConfig& config) {
    if (config.delivery != m_config.delivery) {
        throw EventSystemError(std::format("channel '{}': delivery policy cannot be changed from {} to {}", m_schema.name,
                                           to_string(m_config.delivery), to_string(config.delivery)));
    }
    if (config.domain != m_config.domain) {
        throw EventSystemError(std::format("channel '{}': domain cannot be changed from {} to {}", m_schema.name,
                                           to_string(m_config.domain), to_string(config.domain)));
    }
    m_config = config;
    apply_config();
}

void Channel::apply_config() {
    if (m_config.delivery == Delivery::Coalesced) {
        if (m_config.coalesce_field >= m_schema.fields.size() || m_schema.fields[m_config.coalesce_field].size > 8) {
            throw EventSystemError(std::format("channel '{}': coalesce_field must name a field of at most 8 bytes", m_schema.name));
        }
    }
    m_limit = m_config.max_events_per_tick == 0 ? std::numeric_limits<std::size_t>::max() : m_config.max_events_per_tick;
    m_pending.reserve(m_config.reserve);
    m_ready.reserve(m_config.reserve);
    for (EventBuffer* buffer : {&m_pending, &m_ready, &m_future, &m_kept}) {
        if (buffer->tracing() != m_config.trace) buffer->set_tracing(m_config.trace);
    }
    for (Lane& lane : m_lanes) {
        if (lane.buffer.tracing() != m_config.trace) lane.buffer.set_tracing(m_config.trace);
    }
}

bool Channel::emit_raw(const std::byte* event) {
    if (!has_room()) {
        note_dropped();
        return false;
    }
    m_pending.push_raw(event);
    return true;
}

std::uint32_t Channel::open_lanes(std::uint32_t count) {
    const std::uint32_t base = m_lanes_open;
    m_lanes_open += count;
    while (m_lanes.size() < m_lanes_open) {
        m_lanes.emplace_back(m_schema);
        m_lanes.back().buffer.set_tracing(m_config.trace);
    }
    return base;
}

void Channel::merge_lanes() {
    for (std::uint32_t i = 0; i < m_lanes_open; ++i) {
        EventBuffer& lane = m_lanes[i].buffer;
        const std::size_t room = m_limit - std::min(m_limit, m_pending.size());
        const std::size_t take = std::min(lane.size(), room);
        m_pending.append(lane, 0, take);
        m_total_dropped += lane.size() - take; // бюджет: лишнее с конца, порядок дорожек фиксирован
        lane.clear();
    }
    m_lanes_open = 0;
}

std::uint64_t Channel::key_of(const EventBuffer& buffer, std::size_t index) const noexcept {
    const FieldDesc& field = m_schema.fields[m_config.coalesce_field];
    std::uint64_t key = 0;
    std::memcpy(&key, buffer.field_data(index, m_config.coalesce_field), field.size);
    return key;
}

void Channel::deliver_coalesced() {
    m_keys.clear();
    for (std::size_t i = 0; i < m_pending.size(); ++i) {
        const std::uint64_t key = key_of(m_pending, i);
        if (const auto it = m_keys.find(key); it != m_keys.end()) {
            m_ready.overwrite(it->second, m_pending, i); // последнее значение на месте первого
            ++m_total_coalesced;
        } else {
            m_keys.emplace(key, m_ready.size());
            m_ready.append(m_pending, i, 1);
        }
    }
    m_pending.clear();
}

void Channel::deliver_scheduled() {
    // Сначала созревшие отложенные (они старше), затем обычные события прошлого момента.
    m_kept.clear();
    m_kept_due.clear();
    for (std::size_t i = 0; i < m_future.size(); ++i) {
        if (m_due[i] <= m_time) {
            m_ready.append(m_future, i, 1);
        } else {
            m_kept.append(m_future, i, 1);
            m_kept_due.push_back(m_due[i]);
        }
    }
    m_future.swap(m_kept);
    m_due.swap(m_kept_due);
    m_ready.append(m_pending, 0, m_pending.size());
    m_pending.clear();
}

void Channel::advance() {
    merge_lanes();
    const std::size_t emitted = m_pending.size();
    m_total_emitted += emitted;
    m_peak_per_tick = std::max(m_peak_per_tick, emitted);
    ++m_time;
    m_ready.clear();
    switch (m_config.delivery) {
        case Delivery::Stream: m_ready.swap(m_pending); break; // O(1), без копий
        case Delivery::Coalesced: deliver_coalesced(); break;
        case Delivery::Scheduled: deliver_scheduled(); break;
    }
}

void Channel::clear() noexcept {
    m_pending.clear();
    m_ready.clear();
    m_future.clear();
    m_due.clear();
    for (std::uint32_t i = 0; i < m_lanes_open; ++i) m_lanes[i].buffer.clear();
    m_lanes_open = 0;
}

ChannelStats Channel::stats() const noexcept {
    std::size_t bytes = m_pending.allocated_bytes() + m_ready.allocated_bytes() + m_future.allocated_bytes() + m_kept.allocated_bytes();
    for (const Lane& lane : m_lanes) bytes += lane.buffer.allocated_bytes();
    return ChannelStats{
        .pending = m_pending.size(),
        .scheduled = m_future.size(),
        .lanes = m_lanes.size(),
        .total_coalesced = m_total_coalesced,
        .readable = m_ready.size(),
        .peak_per_tick = m_peak_per_tick,
        .total_emitted = m_total_emitted,
        .total_dropped = m_total_dropped,
        .allocated_bytes = bytes,
    };
}

} // namespace EventSystem
