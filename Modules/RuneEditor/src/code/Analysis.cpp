#include <RuneEditor/Analysis.hpp>

#include <algorithm>
#include <set>

namespace RuneEditor {

using Runes::Code;

std::vector<const NodeProblem*> Analysis::of(NodeId node) const {
    std::vector<const NodeProblem*> out;
    for (const NodeProblem& p : problems) {
        if (p.node == node) out.push_back(&p);
    }
    return out;
}

const NodeProblem* Analysis::worst(NodeId node) const {
    const NodeProblem* best = nullptr;
    for (const NodeProblem& p : problems) {
        if (p.node != node) continue;
        if (!best || (p.error && !best->error)) best = &p;
    }
    return best;
}

std::optional<std::size_t> Analysis::first_rune_of(NodeId node) const {
    const auto it = std::find(source.begin(), source.end(), node);
    if (it == source.end()) return std::nullopt;
    return static_cast<std::size_t>(it - source.begin());
}

namespace {
/// Узлы, достижимые от входа: операторы по next/branch и всё, что они потребляют.
std::set<NodeId> reachable(const Graph& g) {
    std::set<NodeId> seen;
    std::vector<NodeId> stack;
    if (g.entry != Runes::no_node) stack.push_back(g.entry);
    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();
        const GraphNode* n = g.find(id);
        if (!n || !seen.insert(id).second) continue;
        for (const NodeId in : n->inputs) {
            if (in != Runes::no_node) stack.push_back(in);
        }
        if (n->next != Runes::no_node) stack.push_back(n->next);
        if (n->branch != Runes::no_node) stack.push_back(n->branch);
    }
    return seen;
}
} // namespace

Analysis analyze(const Graph& graph, const Runes::Tuning& tuning, std::string name) {
    Analysis a;
    const std::set<NodeId> live = reachable(graph);

    std::set<NodeId> consumed;
    for (const auto& [id, n] : graph.nodes()) {
        for (const NodeId in : n.inputs) consumed.insert(in);
    }
    for (const auto& [id, n] : graph.nodes()) {
        const bool is_live = live.contains(id);
        if (is_live) {
            for (std::size_t i = 0; i < n.inputs.size(); ++i) {
                if (n.inputs[i] == Runes::no_node) a.problems.push_back({id, Code::InputNotConnected, true, std::to_string(i)});
            }
            if (n.rune == Runes::Rune::JmpIf && n.branch == Runes::no_node) a.problems.push_back({id, Code::MissingBranch, true, {}});
        } else if (Runes::is_statement(n.rune)) {
            a.problems.push_back({id, Code::UnreachableNode, false, {}});
        } else if (!consumed.contains(id)) {
            a.problems.push_back({id, Code::UnusedValue, false, {}});
        }
    }

    auto compiled = Runes::compile_mapped(graph, std::move(name));
    if (compiled) {
        a.cost = Runes::estimate_cost(compiled->program, tuning);
        a.program = std::move(compiled->program);
        a.source = std::move(compiled->source);
    } else {
        a.error = compiled.error();
        const NodeId at = a.error->node;
        const bool listed = at != Runes::no_node && std::any_of(a.problems.begin(), a.problems.end(), [&](const NodeProblem& p) { return p.node == at && p.error; });
        if (!listed && at != Runes::no_node) a.problems.push_back({at, a.error->code, true, a.error->detail});
    }
    return a;
}

} // namespace RuneEditor
