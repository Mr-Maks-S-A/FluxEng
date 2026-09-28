#include <EventSystem/Graph/EventGraph.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <queue>

namespace EventSystem {

EventGraph::EventGraph(std::span<const ModuleInfo> modules, std::vector<std::pair<EventId, std::string>> events)
    : m_modules(modules.begin(), modules.end()) {
    m_events.reserve(events.size());
    for (auto& [id, name] : events) {
        m_events.push_back(EventNode{.id = id, .name = std::move(name), .producers = {}, .consumers = {}});
    }

    for (std::size_t m = 0; m < m_modules.size(); ++m) {
        const ModuleId module{static_cast<std::uint32_t>(m)};
        for (EventNode& node : m_events) {
            if (std::ranges::find(m_modules[m].produces, node.id) != m_modules[m].produces.end()) {
                node.producers.push_back(module);
            }
            if (std::ranges::find(m_modules[m].consumes, node.id) != m_modules[m].consumes.end()) {
                node.consumers.push_back(module);
            }
        }
    }
}

const EventNode* EventGraph::find_event(EventId id) const noexcept {
    const auto it = std::ranges::find(m_events, id, &EventNode::id);
    return it == m_events.end() ? nullptr : &*it;
}

std::vector<EventId> EventGraph::unproduced_events() const {
    std::vector<EventId> result;
    for (const EventNode& node : m_events) {
        if (node.producers.empty() && !node.consumers.empty()) {
            result.push_back(node.id);
        }
    }
    return result;
}

std::vector<EventId> EventGraph::unconsumed_events() const {
    std::vector<EventId> result;
    for (const EventNode& node : m_events) {
        if (!node.producers.empty() && node.consumers.empty()) {
            result.push_back(node.id);
        }
    }
    return result;
}

std::vector<EventId> EventGraph::orphan_events() const {
    std::vector<EventId> result;
    for (const EventNode& node : m_events) {
        if (node.producers.empty() && node.consumers.empty()) {
            result.push_back(node.id);
        }
    }
    return result;
}

std::vector<ModuleEdge> EventGraph::module_edges() const {
    std::vector<ModuleEdge> edges;
    for (const EventNode& node : m_events) {
        for (ModuleId from : node.producers) {
            for (ModuleId to : node.consumers) {
                if (from != to) {
                    edges.push_back(ModuleEdge{.from = from, .to = to, .via = node.id});
                }
            }
        }
    }
    return edges;
}

ModuleOrder EventGraph::module_order() const {
    const std::size_t count = m_modules.size();
    std::vector<std::vector<std::size_t>> successors(count);
    std::vector<std::size_t> in_degree(count, 0);

    for (const ModuleEdge& edge : module_edges()) {
        auto& next = successors[edge.from.index];
        // Несколько событий между одной парой модулей — одно ребро.
        if (std::ranges::find(next, edge.to.index) == next.end()) {
            next.push_back(edge.to.index);
            ++in_degree[edge.to.index];
        }
    }

    // Мин-куча по индексу: при равенстве — порядок объявления.
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t i = 0; i < count; ++i) {
        if (in_degree[i] == 0) {
            ready.push(i);
        }
    }

    ModuleOrder result;
    std::vector<bool> placed(count, false);
    while (!ready.empty()) {
        const std::size_t current = ready.top();
        ready.pop();
        placed[current] = true;
        result.order.push_back(ModuleId{static_cast<std::uint32_t>(current)});
        for (std::size_t next : successors[current]) {
            if (--in_degree[next] == 0) {
                ready.push(next);
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!placed[i]) {
            result.cyclic.push_back(ModuleId{static_cast<std::uint32_t>(i)});
        }
    }
    return result;
}

std::string_view EventGraph::event_name(EventId id) const noexcept {
    const EventNode* node = find_event(id);
    return node != nullptr ? std::string_view(node->name) : std::string_view("<unregistered>");
}

std::string EventGraph::to_dot() const {
    std::string out = "digraph EventGraph {\n"
                      "    rankdir=LR;\n"
                      "    node [fontname=\"Helvetica\"];\n";

    for (std::size_t m = 0; m < m_modules.size(); ++m) {
        std::format_to(std::back_inserter(out), "    m{} [label=\"{}\", shape=box, style=filled, fillcolor=\"#dde6f5\"];\n",
                       m, m_modules[m].name);
    }
    for (std::size_t e = 0; e < m_events.size(); ++e) {
        const EventNode& node = m_events[e];
        const bool dangling = node.producers.empty() || node.consumers.empty();
        std::format_to(std::back_inserter(out), "    e{} [label=\"{}\", shape=ellipse{}];\n", e, node.name,
                       dangling ? ", color=\"#c0392b\", fontcolor=\"#c0392b\"" : "");
        for (ModuleId producer : node.producers) {
            std::format_to(std::back_inserter(out), "    m{} -> e{};\n", producer.index, e);
        }
        for (ModuleId consumer : node.consumers) {
            std::format_to(std::back_inserter(out), "    e{} -> m{};\n", e, consumer.index);
        }
    }
    out += "}\n";
    return out;
}

std::string EventGraph::to_text() const {
    std::string out;
    auto module_names = [this](const std::vector<ModuleId>& ids) {
        if (ids.empty()) {
            return std::string("(none)");
        }
        std::string names;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) {
                names += ", ";
            }
            names += m_modules[ids[i].index].name;
        }
        return names;
    };

    for (const ModuleInfo& module : m_modules) {
        std::format_to(std::back_inserter(out), "[{}]\n", module.name);
        for (EventId id : module.produces) {
            const EventNode* node = find_event(id);
            std::format_to(std::back_inserter(out), "  produces {} -> {}\n", event_name(id),
                           node != nullptr ? module_names(node->consumers) : "(none)");
        }
        for (EventId id : module.consumes) {
            const EventNode* node = find_event(id);
            std::format_to(std::back_inserter(out), "  consumes {} <- {}\n", event_name(id),
                           node != nullptr ? module_names(node->producers) : "(none)");
        }
    }
    return out;
}

} // namespace EventSystem
