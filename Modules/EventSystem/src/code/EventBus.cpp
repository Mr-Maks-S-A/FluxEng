#include <EventSystem/Bus/EventBus.hpp>
#include <EventSystem/Core/Error.hpp>

#include <format>
#include <string>
#include <utility>

namespace EventSystem {

namespace {

std::unique_ptr<IChannel> make_channel(const EventSchema& schema, const ChannelConfig& config) {
    switch (config.delivery) {
        case Delivery::Stream: return std::make_unique<StreamChannel>(schema, config);
    }
    throw EventSystemError(std::format("event '{}': unsupported delivery policy", schema.name));
}

} // namespace

IChannel& EventBus::register_schema(const EventSchema& schema, std::optional<ChannelConfig> config) {
    if (auto error = validate_schema(schema)) {
        throw EventSystemError("invalid event schema: " + *error);
    }

    if (auto it = m_index.find(schema.id); it != m_index.end()) {
        IChannel& existing = *m_channels[it->second];
        if (existing.schema().name != schema.name) {
            throw EventSystemError(std::format("event id collision: '{}' and '{}' hash to the same id",
                                               existing.schema().name, schema.name));
        }
        if (existing.schema() != schema) {
            throw EventSystemError(std::format("event '{}' is already registered with a different schema",
                                               schema.name));
        }
        if (config) {
            existing.configure(*config);
        }
        return existing;
    }

    auto channel = make_channel(schema, config.value_or(ChannelConfig{}));
    m_index.emplace(schema.id, m_channels.size());
    m_channels.push_back(std::move(channel));
    return *m_channels.back();
}

IChannel* EventBus::find(EventId id) noexcept {
    const auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : m_channels[it->second].get();
}

const IChannel* EventBus::find(EventId id) const noexcept {
    const auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : m_channels[it->second].get();
}

IChannel* EventBus::find(std::string_view name) noexcept {
    IChannel* channel = find(make_event_id(name));
    return channel != nullptr && channel->name() == name ? channel : nullptr;
}

const IChannel* EventBus::find(std::string_view name) const noexcept {
    const IChannel* channel = find(make_event_id(name));
    return channel != nullptr && channel->name() == name ? channel : nullptr;
}

StreamChannel& EventBus::stream_channel(const EventSchema& schema) {
    return const_cast<StreamChannel&>(std::as_const(*this).stream_channel(schema));
}

const StreamChannel& EventBus::stream_channel(const EventSchema& schema) const {
    const IChannel* channel = find(schema.id);
    if (channel == nullptr) {
        throw EventSystemError(std::format("event '{}' is not registered", schema.name));
    }
    if (channel->schema() != schema) {
        throw EventSystemError(std::format("event '{}': C++ type does not match the registered schema",
                                           schema.name));
    }
    if (channel->delivery() != Delivery::Stream) {
        throw EventSystemError(std::format("event '{}': typed access is only available for Stream channels",
                                           schema.name));
    }
    return static_cast<const StreamChannel&>(*channel);
}

void EventBus::require_declared(ModuleId module, EventId event, Role role) const {
    if (!m_modules.contains(module)) {
        throw EventSystemError("module is not declared");
    }
    const bool declared = role == Role::Producer ? m_modules.produces(module, event)
                                                 : m_modules.consumes(module, event);
    if (!declared) {
        const IChannel* channel = find(event);
        const std::string event_name = channel != nullptr ? std::string(channel->name()) : "<unregistered>";
        throw EventSystemError(std::format("module '{}' did not declare that it {} '{}'",
                                           m_modules.info(module).name,
                                           role == Role::Producer ? "produces" : "consumes", event_name));
    }
}

void EventBus::declare_link(ModuleId module, EventId event, Role role) {
    if (find(event) == nullptr) {
        throw EventSystemError(std::format("module '{}': event {:#x} is not registered",
                                           m_modules.info(module).name, event.value));
    }
    if (role == Role::Producer) {
        m_modules.add_production(module, event);
    } else {
        m_modules.add_consumption(module, event);
    }
}

void EventBus::advance_tick() {
    for (auto& channel : m_channels) {
        channel->advance();
    }
    ++m_tick;
}

void EventBus::clear_all() noexcept {
    for (auto& channel : m_channels) {
        channel->clear();
    }
}

ModuleBuilder EventBus::declare_module(std::string_view name) {
    return ModuleBuilder(*this, m_modules.declare(name));
}

EventGraph EventBus::build_graph() const {
    std::vector<std::pair<EventId, std::string>> events;
    events.reserve(m_channels.size());
    for (const auto& channel : m_channels) {
        events.emplace_back(channel->id(), std::string(channel->name()));
    }
    return EventGraph(m_modules.all(), std::move(events));
}

ModuleBuilder& ModuleBuilder::produces(EventId event) {
    m_bus->declare_link(m_module, event, EventBus::Role::Producer);
    return *this;
}

ModuleBuilder& ModuleBuilder::consumes(EventId event) {
    m_bus->declare_link(m_module, event, EventBus::Role::Consumer);
    return *this;
}

} // namespace EventSystem
