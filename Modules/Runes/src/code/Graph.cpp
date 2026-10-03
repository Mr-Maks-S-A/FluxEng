#include <Runes/Graph.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <set>
#include <sstream>

namespace Runes {

// ------------------------------------------------------------------------ Graph

NodeId Graph::add(Rune rune, std::int32_t value, float x, float y) {
    const NodeId id = ++m_last;
    GraphNode& n = m_nodes[id];
    n.rune = rune, n.value = value, n.x = x, n.y = y;
    return id;
}

void Graph::remove(NodeId id) {
    m_nodes.erase(id);
    if (entry == id) entry = no_node;
    for (auto& [other, n] : m_nodes) {
        if (n.next == id) n.next = no_node;
        if (n.branch == id) n.branch = no_node;
        for (NodeId& in : n.inputs) {
            if (in == id) in = no_node; // слот остаётся (вход отключён), номера остальных входов не сдвигаются
        }
    }
    m_last = m_nodes.empty() ? 0 : m_nodes.rbegin()->first; // номер последнего узла освобождается: нумерация зависит только от содержимого графа
}

GraphNode* Graph::find(NodeId id) noexcept {
    const auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : &it->second;
}
const GraphNode* Graph::find(NodeId id) const noexcept {
    const auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : &it->second;
}

void Graph::set_input(NodeId to, std::size_t slot, NodeId from) {
    GraphNode* n = find(to);
    if (!n) return;
    if (n->inputs.size() <= slot) n->inputs.resize(slot + 1, no_node);
    n->inputs[slot] = from;
}
void Graph::set_next(NodeId from, NodeId to) {
    if (GraphNode* n = find(from)) n->next = to;
}
void Graph::set_branch(NodeId from, NodeId to) {
    if (GraphNode* n = find(from)) n->branch = to;
}

// --------------------------------------------------------------------- compile

namespace {

/// Ширины входов (в ячейках) по рунам; пусто — входов нет.
std::vector<int> widths_table(Rune r) {
    switch (r) {
    case Rune::Add: case Rune::Mul: return {1, 1};
    case Rune::ManaAt: return {3};
    case Rune::Carve: case Rune::Raise: return {3, 1};
    case Rune::Draw: return {3, 1, 1};
    case Rune::JmpIf: return {1};
    default: return {};
    }
}

struct Compiler {
    explicit Compiler(const Graph& g) : graph(g) {}
    const Graph& graph;
    Program program;
    std::vector<NodeId> source; ///< Карта источников: узел, породивший каждую руну.
    NodeId current = no_node;
    std::map<NodeId, std::size_t> label;            ///< Первый байт оператора.
    std::vector<std::pair<std::size_t, NodeId>> fixups; ///< JMP_IF, чей операнд ждёт адрес узла.
    std::set<NodeId> emitted, visiting, checked;
    std::vector<NodeId> queue;
    std::optional<Diagnostic> error;

    void fail(NodeId node, Code code, std::string detail = {}) {
        if (!error) error = Diagnostic{code, 0, node, std::move(detail)};
    }

    void emit(Rune r, std::int32_t operand = 0) {
        program.code.push_back({r, operand});
        source.push_back(current);
    }

    /// Проверка выражения: тип узла, число и ширина входов, отсутствие циклов по данным.
    void check_expression(NodeId id) {
        if (error || checked.contains(id)) return;
        const GraphNode* n = graph.find(id);
        if (!n) return fail(id, Code::MissingNode);
        if (is_statement(n->rune)) return fail(id, Code::StatementAsValue, std::string(rune_name(n->rune)));
        check_inputs(id, *n);
        checked.insert(id);
    }

    void check_inputs(NodeId id, const GraphNode& n) {
        if (n.rune == Rune::Dup || n.rune == Rune::Drop || n.rune == Rune::Count) {
            return fail(id, Code::StackRune, std::string(rune_name(n.rune)));
        }
        const std::vector<int> widths = input_widths(n.rune);
        if (n.inputs.size() != widths.size()) {
            return fail(id, Code::WrongInputCount, std::string(rune_name(n.rune)) + ": нужно входов " + std::to_string(widths.size()) + ", подключено " + std::to_string(n.inputs.size()));
        }
        if (!visiting.insert(id).second) return fail(id, Code::DataCycle);
        for (std::size_t i = 0; i < widths.size() && !error; ++i) {
            if (n.inputs[i] == no_node) return fail(id, Code::InputNotConnected, std::to_string(i));
            check_expression(n.inputs[i]);
            if (error) break;
            const GraphNode* src = graph.find(n.inputs[i]);
            if (value_width(src->rune) != widths[i]) {
                fail(id, Code::WrongInputType, std::string(rune_name(n.rune)) + ": вход " + std::to_string(i) + " ждёт " + (widths[i] == 3 ? "вектор" : "число") + ", подключено " +
                             std::string(rune_name(src->rune)));
            }
        }
        visiting.erase(id);
    }

    void emit_expression(NodeId id) {
        const GraphNode& n = *graph.find(id);
        for (const NodeId in : n.inputs) emit_expression(in);
        current = id;
        emit(n.rune, n.rune == Rune::Push ? n.value : 0);
    }

    void jump_to(NodeId target) { // безусловный переход: «PUSH 1; JMP_IF цель»
        emit(Rune::Push, static_cast<std::int32_t>(Math::Fixed::one_raw));
        fixups.emplace_back(program.code.size(), target);
        emit(Rune::JmpIf);
    }

    void run() {
        if (graph.entry == no_node) return fail(no_node, Code::NoEntry);
        queue.push_back(graph.entry);
        for (std::size_t q = 0; q < queue.size() && !error; ++q) {
            NodeId cur = queue[q];
            if (emitted.contains(cur)) continue; // ветка вела в оператор, уже собранный по цепочке
            Rune last = Rune::Count;
            NodeId last_id = no_node;
            while (cur != no_node && !emitted.contains(cur) && !error) {
                const GraphNode* n = graph.find(cur);
                if (!n) return fail(cur, Code::MissingNode);
                if (!is_statement(n->rune)) return fail(cur, Code::ValueInControlFlow, std::string(rune_name(n->rune)));
                check_inputs(cur, *n);
                if (error) return;
                emitted.insert(cur);
                label[cur] = program.code.size();
                last = n->rune;
                last_id = cur;
                for (const NodeId in : n->inputs) emit_expression(in);
                current = cur;
                emit(n->rune);
                if (n->rune == Rune::Draw) emit(Rune::Drop); // результат DRAW в графе не используется
                if (n->rune == Rune::Halt) break;
                if (n->rune == Rune::JmpIf) {
                    if (n->branch == no_node) return fail(cur, Code::MissingBranch);
                    program.code.back().operand = 0;
                    fixups.emplace_back(program.code.size() - 1, n->branch);
                    queue.push_back(n->branch);
                }
                cur = n->next;
            }
            if (error) return;
            if (last == Rune::Halt) continue;
            current = last_id; // переход и завершающий HALT принадлежат последнему оператору цепочки
            if (cur != no_node) jump_to(cur); // цепочка вливается в уже собранный оператор
            else emit(Rune::Halt);            // цепочка кончилась
        }
        if (error) return;
        for (const auto& [index, target] : fixups) {
            const auto it = label.find(target);
            if (it == label.end()) return fail(target, Code::UnreachableTarget);
            program.code[index].operand = static_cast<std::int32_t>(it->second);
        }
        if (program.code.size() > max_program_length) fail(no_node, Code::ProgramTooLong, std::to_string(program.code.size()));
    }
};

} // namespace

std::vector<int> input_widths(Rune r) { return widths_table(r); }

std::expected<CompiledGraph, Diagnostic> compile_mapped(const Graph& graph, std::string name) {
    Compiler c(graph);
    c.program.name = std::move(name);
    c.run();
    if (c.error) return std::unexpected(*c.error);
    return CompiledGraph{std::move(c.program), std::move(c.source)};
}

std::expected<Program, Diagnostic> compile(const Graph& graph, std::string name) {
    auto mapped = compile_mapped(graph, std::move(name));
    if (!mapped) return std::unexpected(mapped.error());
    return std::move(mapped->program);
}

// ------------------------------------------------------------------ decompile

std::expected<Graph, Diagnostic> decompile(const Program& program) {
    const auto& code = program.code;
    const std::size_t n = code.size();
    const auto err = [](std::size_t pc, Code code, std::string detail = {}) { return std::unexpected(Diagnostic{code, static_cast<int>(pc), no_node, std::move(detail)}); };
    if (n == 0) return std::unexpected(Diagnostic{Code::ProgramEmpty});

    std::vector<bool> leader(n + 1, false);
    leader[0] = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (code[i].rune == Rune::JmpIf) leader[static_cast<std::size_t>(code[i].operand)] = true, leader[i + 1] = true;
    }

    struct Block {
        std::vector<NodeId> statements;
        Rune tail = Rune::Count; ///< Halt, JmpIf или Count — «проваливается» в следующий блок.
        std::size_t fall = 0, target = 0;
    };
    Graph g;
    std::map<std::size_t, Block> blocks;

    struct Cell {
        NodeId id;
        int offset; ///< Номер ячейки внутри значения (0…width−1).
    };
    for (std::size_t start = 0; start < n; ++start) {
        if (!leader[start]) continue;
        Block& block = blocks[start];
        std::vector<Cell> stack;
        std::size_t pc = start;
        bool open = true;
        const auto pop_value = [&](int width) -> std::optional<NodeId> {
            if (stack.size() < static_cast<std::size_t>(width)) return std::nullopt;
            const NodeId id = stack.back().id;
            for (int i = 0; i < width; ++i) {
                const Cell c = stack[stack.size() - 1 - static_cast<std::size_t>(i)];
                if (c.id != id || c.offset != width - 1 - i) return std::nullopt;
            }
            if (value_width(g.find(id)->rune) != width) return std::nullopt;
            stack.resize(stack.size() - static_cast<std::size_t>(width));
            return id;
        };
        while (pc < n && (pc == start || !leader[pc]) && open) {
            const Instruction in = code[pc];
            const Rune r = in.rune;
            switch (r) {
            case Rune::Push: stack.push_back({g.add(Rune::Push, in.operand), 0}); break;
            case Rune::Dup: {
                if (stack.empty() || value_width(g.find(stack.back().id)->rune) != 1) return err(pc, Code::BadStackShape, "DUP работает только с числом");
                stack.push_back(stack.back());
                break;
            }
            case Rune::Drop: {
                if (!pop_value(1)) return err(pc, Code::BadStackShape, "DROP снимает не число");
                break;
            }
            case Rune::Add: case Rune::Mul: {
                const auto b = pop_value(1), a = pop_value(1);
                if (!a || !b) return err(pc, Code::BadStackShape, "нужны два числа");
                const NodeId id = g.add(r);
                g.set_input(id, 0, *a), g.set_input(id, 1, *b);
                stack.push_back({id, 0});
                break;
            }
            case Rune::Caster: case Rune::Aim: case Rune::Target: {
                const NodeId id = g.add(r);
                for (int i = 0; i < 3; ++i) stack.push_back({id, i});
                break;
            }
            case Rune::ManaAt: {
                const auto v = pop_value(3);
                if (!v) return err(pc, Code::BadStackShape, "нужен вектор");
                const NodeId id = g.add(r);
                g.set_input(id, 0, *v);
                stack.push_back({id, 0});
                break;
            }
            case Rune::Carve: case Rune::Raise: case Rune::Draw: {
                const std::size_t count = r == Rune::Draw ? 3 : 2;
                std::vector<NodeId> args(count);
                for (std::size_t i = count; i-- > 0;) {
                    const auto v = pop_value(input_widths(r)[i]);
                    if (!v) return err(pc, Code::BadStackShape, std::string(rune_name(r)) + ": неверные аргументы на стеке");
                    args[i] = *v;
                }
                const NodeId id = g.add(r);
                for (std::size_t i = 0; i < count; ++i) g.set_input(id, i, args[i]);
                block.statements.push_back(id);
                if (r == Rune::Draw) {
                    if (pc + 1 >= n || code[pc + 1].rune != Rune::Drop) return err(pc, Code::DrawNotDropped);
                    ++pc; // DROP входит в оператор
                }
                break;
            }
            case Rune::JmpIf: {
                const auto cond = pop_value(1);
                if (!cond) return err(pc, Code::BadStackShape, "JMP_IF: нужно число-условие");
                const NodeId id = g.add(r);
                g.set_input(id, 0, *cond);
                block.statements.push_back(id);
                block.tail = Rune::JmpIf, block.target = static_cast<std::size_t>(in.operand), block.fall = pc + 1;
                open = false;
                break;
            }
            case Rune::Halt: {
                block.statements.push_back(g.add(Rune::Halt));
                block.tail = Rune::Halt;
                open = false;
                break;
            }
            case Rune::Count: return err(pc, Code::UnknownRune);
            }
            ++pc;
            if (!open && !stack.empty()) return err(pc - 1, Code::StackNotEmpty);
        }
        if (open) {
            if (!stack.empty()) return err(pc - 1, Code::StackNotEmpty);
            block.fall = pc;
        }
    }

    // Связи: вход блока — его первый оператор (или вход следующего блока, если операторов нет).
    std::map<std::size_t, NodeId> entry_memo;
    const auto entry_of = [&](auto&& self, std::size_t start) -> NodeId {
        if (start >= n) return no_node;
        if (const auto it = entry_memo.find(start); it != entry_memo.end()) return it->second;
        const Block& b = blocks.at(start);
        const NodeId id = !b.statements.empty() ? b.statements.front() : self(self, b.fall);
        return entry_memo[start] = id;
    };
    for (auto& [start, b] : blocks) {
        for (std::size_t i = 0; i + 1 < b.statements.size(); ++i) g.set_next(b.statements[i], b.statements[i + 1]);
        if (b.statements.empty()) continue;
        const NodeId last = b.statements.back();
        if (b.tail == Rune::JmpIf) {
            g.set_next(last, entry_of(entry_of, b.fall));
            g.set_branch(last, entry_of(entry_of, b.target));
            if (g.find(last)->branch == no_node) return std::unexpected(Diagnostic{Code::JumpToEnd, 0, last});
        } else if (b.tail != Rune::Halt) {
            g.set_next(last, entry_of(entry_of, b.fall));
        }
    }
    g.entry = entry_of(entry_of, 0);
    return g;
}

// ------------------------------------------------------------------ serialization

std::string serialize(const Graph& graph) {
    std::ostringstream out;
    out << "# FluxEng rune graph\nentry " << graph.entry << '\n';
    for (const auto& [id, n] : graph.nodes()) {
        out << "node " << id << ' ' << rune_name(n.rune);
        if (n.rune == Rune::Push) {
            const std::string text = format_fixed(n.value);
            if (parse_fixed(text).value_or(0) == n.value) out << " value " << text;
            else out << " raw " << n.value; // точная запись для чисел без короткого десятичного вида
        }
        if (!n.inputs.empty()) {
            out << " in";
            for (const NodeId in : n.inputs) out << ' ' << in;
        }
        if (n.next != no_node) out << " next " << n.next;
        if (n.branch != no_node) out << " branch " << n.branch;
        out << " at " << n.x << ' ' << n.y << '\n';
    }
    return out.str();
}

std::expected<Graph, Diagnostic> parse_graph(std::string_view text) {
    Graph g;
    std::map<NodeId, GraphNode> parsed;
    NodeId entry = no_node, max_id = 0;
    int line_number = 0;
    const auto fail = [&](Code code, std::string detail = {}) { return std::unexpected(Diagnostic{code, line_number, no_node, std::move(detail)}); };
    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line)) {
        ++line_number;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream words(line);
        std::string key;
        if (!(words >> key)) continue;
        if (key == "entry") {
            if (!(words >> entry)) return fail(Code::BadField, "entry: ожидался номер узла");
            continue;
        }
        if (key != "node") return fail(Code::UnknownKeyword, key);
        NodeId id = 0;
        std::string mnemonic;
        if (!(words >> id >> mnemonic) || id == no_node) return fail(Code::BadField, "node: ожидалось «node <номер> <РУНА>»");
        if (parsed.contains(id)) return fail(Code::DuplicateNode, std::to_string(id));
        std::ranges::transform(mnemonic, mnemonic.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        GraphNode node;
        bool known = false;
        for (int r = 0; r < static_cast<int>(Rune::Count); ++r) {
            if (rune_name(static_cast<Rune>(r)) == mnemonic) node.rune = static_cast<Rune>(r), known = true;
        }
        if (!known) return fail(Code::UnknownRune, mnemonic);
        std::string field;
        while (words >> field) {
            if (field == "value") {
                std::string number;
                words >> number;
                const auto v = parse_fixed(number);
                if (!v) return fail(v.error().code, v.error().detail);
                node.value = *v;
            } else if (field == "raw") {
                if (!(words >> node.value)) return fail(Code::BadField, "raw: ожидалось целое");
            } else if (field == "in") {
                std::streampos before = words.tellg();
                NodeId in;
                while (words >> in) before = words.tellg(), node.inputs.push_back(in);
                words.clear();
                words.seekg(before);
            } else if (field == "next") {
                if (!(words >> node.next)) return fail(Code::BadField, "next: ожидался номер");
            } else if (field == "branch") {
                if (!(words >> node.branch)) return fail(Code::BadField, "branch: ожидался номер");
            } else if (field == "at") {
                if (!(words >> node.x >> node.y)) return fail(Code::BadField, "at: ожидались две координаты");
            } else {
                return fail(Code::BadField, "неизвестное поле: " + field);
            }
        }
        parsed[id] = std::move(node);
        max_id = std::max(max_id, id);
    }
    // Перенос в Graph с сохранением номеров: добавляем узлы по порядку и пропускаем дыры.
    for (NodeId id = 1; id <= max_id; ++id) {
        const NodeId made = g.add(Rune::Halt);
        if (const auto it = parsed.find(id); it != parsed.end()) *g.find(made) = it->second;
        else g.remove(made);
    }
    g.entry = entry;
    return g;
}

} // namespace Runes
