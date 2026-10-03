#include <RuneEditor/Layout.hpp>

#include <algorithm>
#include <deque>
#include <map>

namespace RuneEditor {

int auto_layout(Graph& graph, const LayoutOptions& options) {
    std::map<NodeId, int> col; // номер колонки (выражения — отрицательные до нормализации)
    // 1. Операторы: ширина по шагам от входа (BFS), недостижимые — после.
    std::deque<NodeId> queue;
    int rank_max = 0;
    auto visit = [&](NodeId id, int rank) {
        if (id != Runes::no_node && graph.find(id) && !col.contains(id)) {
            col[id] = rank * 2;
            rank_max = std::max(rank_max, rank);
            queue.push_back(id);
        }
    };
    visit(graph.entry, 0);
    for (;;) {
        while (!queue.empty()) {
            const NodeId id = queue.front();
            queue.pop_front();
            const GraphNode& n = *graph.find(id);
            visit(n.next, col[id] / 2 + 1);
            visit(n.branch, col[id] / 2 + 1);
        }
        NodeId rest = Runes::no_node;
        for (const auto& [id, n] : graph.nodes()) {
            if (Runes::is_statement(n.rune) && !col.contains(id)) { rest = id; break; }
        }
        if (rest == Runes::no_node) break;
        visit(rest, rank_max + 1);
    }
    // 2. Выражения: левее самого левого потребителя (до неподвижной точки; граф без циклов данных сходится за N проходов).
    std::map<NodeId, int> expr;
    for (std::size_t pass = 0; pass <= graph.nodes().size(); ++pass) {
        bool changed = false;
        for (const auto& [id, n] : graph.nodes()) {
            int at;
            if (Runes::is_statement(n.rune)) at = col[id];
            else if (const auto it = expr.find(id); it != expr.end()) at = it->second;
            else continue;
            for (const NodeId in : n.inputs) {
                const GraphNode* src = graph.find(in);
                if (!src || Runes::is_statement(src->rune)) continue;
                const auto cur = expr.find(in);
                if (cur == expr.end() || at - 1 < cur->second) expr[in] = at - 1, changed = true;
            }
        }
        if (!changed) break;
    }
    // Выражения без потребителей — в колонку 0.
    for (const auto& [id, n] : graph.nodes()) {
        if (Runes::is_statement(n.rune)) continue;
        col[id] = expr.contains(id) ? expr[id] : 0;
    }
    const int min_col = std::min_element(col.begin(), col.end(), [](auto& a, auto& b) { return a.second < b.second; })->second;
    std::map<int, int> rows; // колонка → сколько узлов уже стоит
    int moved = 0;
    std::vector<NodeId> ids;
    for (const auto& [id, n] : graph.nodes()) ids.push_back(id);
    for (const NodeId id : ids) {
        GraphNode& n = *graph.find(id);
        const int c = col[id] - min_col;
        const float x = options.origin.x + static_cast<float>(c) * options.column;
        const float y = options.origin.y + static_cast<float>(rows[c]++) * options.row;
        if (n.x != x || n.y != y) ++moved;
        n.x = x, n.y = y;
    }
    return moved;
}

} // namespace RuneEditor
