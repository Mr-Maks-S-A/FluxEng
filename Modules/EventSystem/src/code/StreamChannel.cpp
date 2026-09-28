#include <EventSystem/Channel/StreamChannel.hpp>
#include <EventSystem/Core/Error.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace EventSystem {

std::string_view to_string(Delivery delivery) noexcept {
    switch (delivery) {
        case Delivery::Stream: return "Stream";
    }
    return "unknown";
}

StreamChannel::StreamChannel(EventSchema schema, const ChannelConfig& config)
    : m_schema(std::move(schema)), m_config(config), m_pending(m_schema), m_ready(m_schema) {
    if (config.delivery != Delivery::Stream) {
        throw EventSystemError(std::format("StreamChannel '{}': delivery must be Stream", m_schema.name));
    }
    apply_config();
}

void StreamChannel::configure(const ChannelConfig& config) {
    if (config.delivery != Delivery::Stream) {
        throw EventSystemError(std::format("channel '{}': delivery policy cannot be changed from Stream to {}",
                                           m_schema.name, to_string(config.delivery)));
    }
    m_config = config;
    apply_config();
}

void StreamChannel::apply_config() {
    m_limit = m_config.max_events_per_tick == 0 ? std::numeric_limits<std::size_t>::max()
                                                : m_config.max_events_per_tick;
    m_pending.reserve(m_config.reserve);
    m_ready.reserve(m_config.reserve);
}

bool StreamChannel::emit_raw(const std::byte* event) {
    if (!has_room()) {
        note_dropped();
        return false;
    }
    m_pending.push_raw(event);
    return true;
}

void StreamChannel::advance() {
    const std::size_t emitted = m_pending.size();
    m_total_emitted += emitted;
    m_peak_per_tick = std::max(m_peak_per_tick, emitted);

    m_ready.clear();
    m_ready.swap(m_pending);
}

void StreamChannel::clear() noexcept {
    m_pending.clear();
    m_ready.clear();
}

ChannelStats StreamChannel::stats() const noexcept {
    return ChannelStats{
        .pending = m_pending.size(),
        .readable = m_ready.size(),
        .peak_per_tick = m_peak_per_tick,
        .total_emitted = m_total_emitted,
        .total_dropped = m_total_dropped,
        .allocated_bytes = m_pending.allocated_bytes() + m_ready.allocated_bytes(),
    };
}

} // namespace EventSystem
