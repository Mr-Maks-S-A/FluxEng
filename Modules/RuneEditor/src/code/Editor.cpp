#include <RuneEditor/Editor.hpp>

#include <algorithm>

namespace RuneEditor {

using Runes::Rune;

namespace {
constexpr std::size_t history_limit = 200;

/// Выражение `from` зависит от `target` через входы данных?
bool depends_on(const Graph& g, NodeId from, NodeId target, int depth = 0) {
    if (from == target) return true;
    if (depth > 256) return true; // защита от цикла в загруженном графе
    const GraphNode* n = g.find(from);
    if (!n) return false;
    for (const NodeId in : n->inputs) {
        if (in != Runes::no_node && depends_on(g, in, target, depth + 1)) return true;
    }
    return false;
}
} // namespace

void Editor::reset(Graph graph) {
    m_graph = std::move(graph);
    m_undo.clear(), m_redo.clear(), m_origin.clear(), m_selection.clear();
    m_depth = 0, m_group_pushed = false;
    changed();
}

void Editor::push_history() {
    if (m_depth > 0) {
        if (m_group_pushed) return;
        m_group_pushed = true;
    }
    m_undo.push_back(m_graph);
    if (m_undo.size() > history_limit) m_undo.erase(m_undo.begin());
    m_redo.clear();
}

bool Editor::can_connect(const PortRef& a, const PortRef& b) const {
    const GraphNode* na = m_graph.find(a.node);
    const GraphNode* nb = m_graph.find(b.node);
    if (!na || !nb) return false;
    // Данные: выход одного, вход другого — в любом порядке.
    if ((a.kind == PortKind::Output && b.kind == PortKind::Input) || (a.kind == PortKind::Input && b.kind == PortKind::Output)) {
        const PortRef& out = a.kind == PortKind::Output ? a : b;
        const PortRef& in = a.kind == PortKind::Input ? a : b;
        const GraphNode& src = *m_graph.find(out.node);
        const GraphNode& dst = *m_graph.find(in.node);
        if (!has_port(src, PortKind::Output) || !has_port(dst, PortKind::Input, in.index)) return false;
        if (out.node == in.node) return false;
        if (Runes::input_widths(dst.rune)[static_cast<std::size_t>(in.index)] != Runes::value_width(src.rune)) return false;
        return !depends_on(m_graph, out.node, in.node);
    }
    // Управление: Next/Branch → любой оператор (в том числе самого себя: цикл).
    const PortRef* ctl = (a.kind == PortKind::Next || a.kind == PortKind::Branch) ? &a : (b.kind == PortKind::Next || b.kind == PortKind::Branch) ? &b : nullptr;
    if (!ctl) return false;
    const PortRef& other = ctl == &a ? b : a;
    const GraphNode& from = *m_graph.find(ctl->node);
    const GraphNode& to = *m_graph.find(other.node);
    return has_port(from, ctl->kind) && Runes::is_statement(to.rune) && other.kind != PortKind::Next && other.kind != PortKind::Branch;
}

OpResult Editor::execute(const Op& op) {
    using K = Op::Kind;
    auto done = [&](OpResult r) {
        if (r.ok && m_observer) m_observer(op);
        return r;
    };
    switch (op.kind) {
    case K::Begin:
        if (m_depth++ == 0) m_group_pushed = false;
        return done({true});
    case K::End:
        if (m_depth == 0) return {};
        if (--m_depth == 0) m_group_pushed = false;
        return done({true});
    case K::Undo: {
        if (m_undo.empty() || m_depth > 0) return {};
        m_redo.push_back(std::move(m_graph));
        m_graph = std::move(m_undo.back());
        m_undo.pop_back();
        std::erase_if(m_selection, [&](NodeId id) { return !m_graph.find(id); });
        changed();
        return done({true});
    }
    case K::Redo: {
        if (m_redo.empty() || m_depth > 0) return {};
        m_undo.push_back(std::move(m_graph));
        m_graph = std::move(m_redo.back());
        m_redo.pop_back();
        std::erase_if(m_selection, [&](NodeId id) { return !m_graph.find(id); });
        changed();
        return done({true});
    }
    default: break;
    }

    // Правки графа: сначала проверка, потом снимок и изменение.
    GraphNode* n = m_graph.find(op.node);
    switch (op.kind) {
    case K::AddNode:
        if (op.rune >= Rune::Count || op.rune == Rune::Dup || op.rune == Rune::Drop) return {};
        break;
    case K::RemoveNode: case K::MoveNode: case K::SetValue: case K::SetEntry:
        if (!n) return {};
        if (op.kind == K::SetEntry && !Runes::is_statement(n->rune)) return {};
        if (op.kind == K::SetValue && n->rune != Rune::Push) return {};
        break;
    case K::SetInput: {
        if (!n || op.slot < 0 || op.slot >= input_count(*n)) return {};
        if (op.other != Runes::no_node && !can_connect({op.other, PortKind::Output, 0}, {op.node, PortKind::Input, op.slot})) return {};
        break;
    }
    case K::SetNext: case K::SetBranch: {
        if (!n || !has_port(*n, op.kind == K::SetNext ? PortKind::Next : PortKind::Branch)) return {};
        if (op.other != Runes::no_node) {
            const GraphNode* t = m_graph.find(op.other);
            if (!t || !Runes::is_statement(t->rune)) return {};
        }
        break;
    }
    default: return {};
    }

    push_history();
    OpResult result{true, op.node};
    switch (op.kind) {
    case K::AddNode: {
        result.node = m_graph.add(op.rune, op.value, op.x, op.y);
        m_graph.find(result.node)->inputs.assign(static_cast<std::size_t>(input_count(*m_graph.find(result.node))), Runes::no_node);
        break;
    }
    case K::RemoveNode: m_graph.remove(op.node), m_selection.erase(op.node); break;
    case K::MoveNode: n->x = op.x, n->y = op.y; break;
    case K::SetValue: n->value = op.value; break;
    case K::SetEntry: m_graph.entry = op.node; break;
    case K::SetInput: m_graph.set_input(op.node, static_cast<std::size_t>(op.slot), op.other); break;
    case K::SetNext: m_graph.set_next(op.node, op.other); break;
    case K::SetBranch: m_graph.set_branch(op.node, op.other); break;
    default: break;
    }
    changed();
    return done(result);
}

NodeId Editor::add_node(Rune rune, Vec2 at, std::int32_t value) {
    return execute({.kind = Op::Kind::AddNode, .rune = rune, .value = value, .x = at.x, .y = at.y}).node;
}
bool Editor::remove_node(NodeId id) { return execute({.kind = Op::Kind::RemoveNode, .node = id}).ok; }
bool Editor::move_node(NodeId id, Vec2 to) { return execute({.kind = Op::Kind::MoveNode, .node = id, .x = to.x, .y = to.y}).ok; }
bool Editor::set_value(NodeId id, std::int32_t value) { return execute({.kind = Op::Kind::SetValue, .node = id, .value = value}).ok; }
bool Editor::set_entry(NodeId id) { return execute({.kind = Op::Kind::SetEntry, .node = id}).ok; }
void Editor::begin() { execute({.kind = Op::Kind::Begin}); }
void Editor::end() { execute({.kind = Op::Kind::End}); }

bool Editor::disconnect(const EdgeRef& e) {
    switch (e.kind) {
    case PortKind::Output: return execute({.kind = Op::Kind::SetInput, .slot = e.slot, .node = e.to}).ok;
    case PortKind::Next: return execute({.kind = Op::Kind::SetNext, .node = e.from}).ok;
    case PortKind::Branch: return execute({.kind = Op::Kind::SetBranch, .node = e.from}).ok;
    default: return false;
    }
}

bool Editor::connect(const PortRef& a, const PortRef& b) {
    if (!can_connect(a, b)) return false;
    if (a.kind == PortKind::Output || a.kind == PortKind::Input) {
        const PortRef& out = a.kind == PortKind::Output ? a : b;
        const PortRef& in = a.kind == PortKind::Input ? a : b;
        return execute({.kind = Op::Kind::SetInput, .slot = in.index, .node = in.node, .other = out.node}).ok;
    }
    const bool a_is_ctl = a.kind == PortKind::Next || a.kind == PortKind::Branch;
    const PortRef& ctl = a_is_ctl ? a : b;
    const PortRef& target = a_is_ctl ? b : a;
    return execute({.kind = ctl.kind == PortKind::Next ? Op::Kind::SetNext : Op::Kind::SetBranch, .node = ctl.node, .other = target.node}).ok;
}

void Editor::preview_move(NodeId id, Vec2 to) {
    GraphNode* n = m_graph.find(id);
    if (!n) return;
    m_origin.try_emplace(id, Vec2{n->x, n->y});
    n->x = to.x, n->y = to.y;
}

void Editor::cancel_preview() {
    for (const auto& [id, pos] : m_origin) {
        if (GraphNode* n = m_graph.find(id)) n->x = pos.x, n->y = pos.y;
    }
    m_origin.clear();
}

void Editor::commit_preview() {
    std::vector<std::pair<NodeId, Vec2>> finals;
    for (const auto& [id, pos] : m_origin) {
        const GraphNode* n = m_graph.find(id);
        if (n && (n->x != pos.x || n->y != pos.y)) finals.push_back({id, {n->x, n->y}});
    }
    cancel_preview(); // вернуть исходные положения: снимок истории должен быть «до»
    if (finals.empty()) return;
    begin();
    for (const auto& [id, pos] : finals) move_node(id, pos);
    end();
}

void Editor::select(NodeId id, bool additive) {
    if (!additive) m_selection.clear();
    if (m_graph.find(id)) m_selection.insert(id);
}

void Editor::select_in(const Rect& rect, bool additive) {
    if (!additive) m_selection.clear();
    for (const auto& [id, n] : m_graph.nodes()) {
        if (rect.contains({n.x, n.y})) m_selection.insert(id);
    }
}

void Editor::select_all() {
    for (const auto& [id, n] : m_graph.nodes()) m_selection.insert(id);
}

void Editor::erase_selection() {
    if (m_selection.empty()) return;
    const std::vector<NodeId> ids(m_selection.begin(), m_selection.end());
    begin();
    for (const NodeId id : ids) remove_node(id);
    end();
}

} // namespace RuneEditor
