#include <EventSystem/Bus/EventBus.hpp>
#include <EventSystem/Core/Error.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace EventSystem {

namespace {

std::unique_ptr<Channel> make_channel(const EventSchema& schema, const ChannelConfig& config) {
    return std::make_unique<Channel>(schema, config); // одна реализация на все политики и домены
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
    channel->set_index(static_cast<std::uint32_t>(m_channels.size()));
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

Channel& EventBus::channel_of(const EventSchema& schema) {
    return const_cast<Channel&>(std::as_const(*this).channel_of(schema));
}

const Channel& EventBus::channel_of(const EventSchema& schema) const {
    const IChannel* channel = find(schema.id);
    if (channel == nullptr) {
        throw EventSystemError(std::format("event '{}' is not registered", schema.name));
    }
    if (channel->schema() != schema) {
        throw EventSystemError(std::format("event '{}': C++ type does not match the registered schema",
                                           schema.name));
    }
    return static_cast<const Channel&>(*channel);
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

void EventBus::advance(Domain domain) {
    for (auto& channel : m_channels) {
        if (channel->domain() != domain) continue;
        channel->advance();
        if (channel->config().trace) record_trace(*channel);
    }
}

void EventBus::advance_tick() {
    advance(Domain::Tick);
    ++m_tick;
}

void EventBus::advance_frame() {
    advance(Domain::Frame);
    ++m_frame;
}

// ================================================================= дерево причин

void EventBus::set_trace_capacity(std::size_t records) {
    m_journal_capacity = records;
    m_journal.clear();
    m_journal_head = 0;
}

void EventBus::record_trace(const Channel& channel) {
    if (m_journal_capacity == 0) return;
    const EventBuffer& ready = channel.ready();
    for (std::size_t i = 0; i < ready.size(); ++i) {
        const TraceRecord record{channel.ref(i), ready.cause(i), channel.schema().id};
        if (m_journal.size() < m_journal_capacity) {
            m_journal.push_back(record);
        } else {
            m_journal[m_journal_head] = record;
            m_journal_head = (m_journal_head + 1) % m_journal_capacity;
        }
    }
}

std::vector<TraceRecord> EventBus::trace_journal() const {
    std::vector<TraceRecord> out;
    out.reserve(m_journal.size());
    for (std::size_t i = 0; i < m_journal.size(); ++i) out.push_back(m_journal[(m_journal_head + i) % m_journal.size()]);
    return out;
}

std::vector<TraceRecord> EventBus::cause_chain(EventRef ref) const {
    std::vector<TraceRecord> chain;
    const auto find = [&](EventRef r) -> const TraceRecord* {
        for (const TraceRecord& record : m_journal) {
            if (record.ref == r) return &record;
        }
        return nullptr;
    };
    for (const TraceRecord* record = find(ref); record != nullptr && chain.size() < 256; record = find(record->cause)) {
        chain.push_back(*record);
        if (!record->cause.valid()) break;
    }
    std::ranges::reverse(chain);
    return chain;
}

std::vector<TraceRecord> EventBus::effects(EventRef ref) const {
    std::vector<TraceRecord> out;
    for (const TraceRecord& record : trace_journal()) {
        if (record.cause == ref) out.push_back(record);
    }
    return out;
}

std::string EventBus::describe(EventRef ref) const {
    const std::string name = ref.channel() < m_channels.size() ? std::string(m_channels[ref.channel()]->name()) : "?";
    return std::format("{}@{}#{}", name, ref.time(), ref.index());
}

std::string EventBus::trace_tree(EventRef root, int depth) const {
    std::string out;
    const auto walk = [&](const auto& self, EventRef ref, int level) -> void {
        out += std::string(static_cast<std::size_t>(level) * 2, ' ') + describe(ref) + "\n";
        if (level >= depth) return;
        for (const TraceRecord& child : effects(ref)) self(self, child.ref, level + 1);
    };
    walk(walk, root, 0);
    return out;
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
