#include <RuneEditor/Geometry.hpp>

#include <algorithm>
#include <numbers>

namespace RuneEditor {

using Runes::Rune;

int input_count(const GraphNode& node) { return static_cast<int>(Runes::input_widths(node.rune).size()); }

bool has_port(const GraphNode& node, PortKind kind, int index) {
    switch (kind) {
    case PortKind::Output: return !Runes::is_statement(node.rune);
    case PortKind::Input: return index >= 0 && index < input_count(node);
    case PortKind::Next: return Runes::is_statement(node.rune) && node.rune != Rune::Halt;
    case PortKind::Branch: return node.rune == Rune::JmpIf;
    }
    return false;
}

namespace {
Vec2 on_ring(const GraphNode& n, float degrees) {
    const float a = degrees * std::numbers::pi_v<float> / 180.0f;
    return {n.x + node_radius * std::cos(a), n.y + node_radius * std::sin(a)}; // y вниз: 90° — вниз, 180° — влево
}
} // namespace

std::optional<Vec2> port_position(const Graph& graph, const PortRef& port) {
    const GraphNode* n = graph.find(port.node);
    if (!n || !has_port(*n, port.kind, port.index)) return std::nullopt;
    switch (port.kind) {
    case PortKind::Output:
    case PortKind::Next: return on_ring(*n, 0.0f);
    case PortKind::Branch: return on_ring(*n, 90.0f);
    case PortKind::Input: {
        const int count = input_count(*n);
        const float step = 36.0f; // градусов между соседними входами
        const float first = 180.0f - step * static_cast<float>(count - 1) * 0.5f;
        return on_ring(*n, first + step * static_cast<float>(port.index));
    }
    }
    return std::nullopt;
}

std::vector<PortRef> ports_of(NodeId id, const GraphNode& node) {
    std::vector<PortRef> out;
    for (int i = 0; i < input_count(node); ++i) out.push_back({id, PortKind::Input, i});
    for (const PortKind k : {PortKind::Output, PortKind::Next, PortKind::Branch}) {
        if (has_port(node, k)) out.push_back({id, k, 0});
    }
    return out;
}

std::vector<EdgeRef> edges_of(const Graph& graph) {
    std::vector<EdgeRef> out;
    for (const auto& [id, node] : graph.nodes()) {
        for (std::size_t i = 0; i < node.inputs.size(); ++i) {
            if (node.inputs[i] != Runes::no_node) out.push_back({node.inputs[i], PortKind::Output, id, static_cast<int>(i)});
        }
        if (node.next != Runes::no_node) out.push_back({id, PortKind::Next, node.next, 0});
        if (node.branch != Runes::no_node) out.push_back({id, PortKind::Branch, node.branch, 0});
    }
    return out;
}

std::vector<Vec2> edge_polyline(Vec2 from, Vec2 to, int segments) {
    // Кубическая кривая Безье: касательные горизонтальные, длина тем больше, чем дальше концы (рёбра «текут» слева направо).
    const float reach = std::max(40.0f, std::abs(to.x - from.x) * 0.5f);
    const Vec2 c1{from.x + reach, from.y}, c2{to.x - reach, to.y};
    std::vector<Vec2> pts;
    pts.reserve(static_cast<std::size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments), u = 1.0f - t;
        pts.push_back(from * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + to * (t * t * t));
    }
    return pts;
}

std::optional<std::pair<Vec2, Vec2>> edge_endpoints(const Graph& graph, const EdgeRef& edge) {
    const auto a = port_position(graph, {edge.from, edge.kind, 0});
    const auto b = port_position(graph, {edge.to, PortKind::Input, edge.slot});
    if (edge.kind == PortKind::Output) {
        if (a && b) return std::pair{*a, *b};
        return std::nullopt;
    }
    // Управление приходит в «левую» точку оператора-цели (его кольцо слева, как у входа данных).
    const GraphNode* to = graph.find(edge.to);
    if (!a || !to) return std::nullopt;
    return std::pair{*a, Vec2{to->x - node_radius, to->y}};
}

namespace {
float distance_to_segment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    const float t = len2 > 0.0f ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
    return (p - (a + ab * t)).length();
}
} // namespace

Pick pick(const Graph& graph, Vec2 world, float zoom) {
    const float scale = 1.0f / std::max(zoom, 0.05f);
    // 1. Порты — ближайший в пределах радиуса (радиус постоянен на экране).
    Pick best;
    float best_distance = port_hit_radius * scale;
    for (const auto& [id, node] : graph.nodes()) {
        if ((world - Vec2{node.x, node.y}).length() > node_radius + port_hit_radius * scale) continue;
        for (const PortRef& port : ports_of(id, node)) {
            const float d = (world - *port_position(graph, port)).length();
            if (d < best_distance) {
                best_distance = d;
                best = {Pick::Kind::Port, id, port, {}};
            }
        }
    }
    if (best.kind == Pick::Kind::Port) return best;
    // 2. Узлы: верхний (больший номер) под курсором.
    for (auto it = graph.nodes().rbegin(); it != graph.nodes().rend(); ++it) {
        if ((world - Vec2{it->second.x, it->second.y}).length() <= node_radius) return {Pick::Kind::Node, it->first, {}, {}};
    }
    // 3. Рёбра: ближайшая точка кривой в пределах нескольких пикселей.
    float edge_distance = 7.0f * scale;
    Pick edge_pick;
    for (const EdgeRef& e : edges_of(graph)) {
        const auto ends = edge_endpoints(graph, e);
        if (!ends) continue;
        const auto line = edge_polyline(ends->first, ends->second);
        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
            const float d = distance_to_segment(world, line[i], line[i + 1]);
            if (d < edge_distance) {
                edge_distance = d;
                edge_pick = {Pick::Kind::Edge, Runes::no_node, {}, e};
            }
        }
    }
    return edge_pick;
}

Rect bounds_of(const Graph& graph) {
    if (graph.nodes().empty()) return {};
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const auto& [id, n] : graph.nodes()) {
        x0 = std::min(x0, n.x - node_radius), y0 = std::min(y0, n.y - node_radius);
        x1 = std::max(x1, n.x + node_radius), y1 = std::max(y1, n.y + node_radius);
    }
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}

} // namespace RuneEditor
