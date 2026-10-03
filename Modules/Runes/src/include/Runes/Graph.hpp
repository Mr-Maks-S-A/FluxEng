#pragma once
/**
 * @file Graph.hpp
 * @brief Граф рун — второе представление заклинания; компилируется в тот же байт-код `Program`, что и текст.
 *
 * ```
 *   текст .rune ──parse_program──┐
 *                                ├──► Program (байт-код, неизменный) ──► стековая машина
 *   граф .rungraph ──compile─────┘            ▲
 *        ▲                                    │
 *        └──────────── decompile ─────────────┘   (по возможности)
 * ```
 *
 * Модель — поток данных + поток управления:
 * - **выражения** (PUSH, ADD, MUL, CASTER, AIM, TARGET, MANA_AT) дают значение; их входы — рёбра `inputs`
 *   (порядок = порядок на стеке). Один узел-выражение может кормить несколько потребителей;
 * - **операторы** (CARVE, RAISE, DRAW, JMP_IF, HALT) исполняются по цепочке `next`; JMP_IF ещё имеет `branch`
 *   (куда идти, если условие ≠ 0). Циклы — это рёбра `next`/`branch` назад;
 * - DUP и DROP — операции стека, в графе их нет (повторное использование узла — это ребро);
 * - `x, y` — положение узла в визуальном редакторе (на смысл не влияют, но сохраняются).
 *
 * Ограничения (расширятся вместе с функциями и переменными): значение не живёт на стеке между операторами,
 * поэтому программы со счётчиком на стеке (как `tunnel.rune`) в граф не разворачиваются.
 *
 * Формат файла `.rungraph` — текст, по узлу на строку:
 * @code
 * entry 3
 * node 1 TARGET at 40 20
 * node 2 PUSH value 2 at 40 80
 * node 3 CARVE in 1 2 next 4 at 200 40
 * node 4 HALT at 360 40
 * @endcode
 */

#include <Runes/Program.hpp>

#include <map>
#include <string_view>

namespace Runes {

struct GraphNode {
    Rune rune = Rune::Halt;
    std::int32_t value = 0;      ///< PUSH: Fixed.raw.
    std::vector<NodeId> inputs;  ///< Входы данных по порядку.
    NodeId next = no_node;       ///< Следующий оператор (нет — конец программы).
    NodeId branch = no_node;     ///< JMP_IF: куда при условии ≠ 0.
    float x = 0.0f, y = 0.0f;    ///< Положение в редакторе.
    [[nodiscard]] friend bool operator==(const GraphNode&, const GraphNode&) = default;
};

class Graph {
public:
    NodeId entry = no_node; ///< Первый оператор.

    /// @brief Новый узел; возвращает его идентификатор.
    NodeId add(Rune rune, std::int32_t value = 0, float x = 0.0f, float y = 0.0f);
    /// @brief Удаляет узел и все рёбра, которые на него указывали (вход-источник становится отключённым, номера входов не сдвигаются).
    /// Следующий `add` получает номер «наибольший существующий + 1»: граф с тем же содержимым нумеруется одинаково (важно для повтора правок).
    void remove(NodeId id);
    [[nodiscard]] GraphNode* find(NodeId id) noexcept;
    [[nodiscard]] const GraphNode* find(NodeId id) const noexcept;
    [[nodiscard]] const std::map<NodeId, GraphNode>& nodes() const noexcept { return m_nodes; }

    /// @brief Вход `slot` узла `to` берётся из узла `from`.
    void set_input(NodeId to, std::size_t slot, NodeId from);
    void set_next(NodeId from, NodeId to);
    void set_branch(NodeId from, NodeId to);

    [[nodiscard]] friend bool operator==(const Graph&, const Graph&) = default;

private:
    std::map<NodeId, GraphNode> m_nodes;
    NodeId m_last = 0;
};

[[nodiscard]] constexpr bool is_statement(Rune r) noexcept {
    return r == Rune::Carve || r == Rune::Raise || r == Rune::Draw || r == Rune::JmpIf || r == Rune::Halt;
}
/// @brief Ширина значения выражения в ячейках стека: 3 у векторов (CASTER, AIM, TARGET), иначе 1.
[[nodiscard]] constexpr int value_width(Rune r) noexcept { return r == Rune::Caster || r == Rune::Aim || r == Rune::Target ? 3 : 1; }

/// @brief Ширины входов (в ячейках стека) руны: у ADD — {1, 1}, у CARVE — {3, 1} и т. д.; пусто — входов нет (редактор подсказывает по ней, что подключать).
[[nodiscard]] std::vector<int> input_widths(Rune rune);

/// @brief Проверяет граф и собирает байт-код. Недостижимые от `entry` узлы игнорируются.
[[nodiscard]] std::expected<Program, Diagnostic> compile(const Graph& graph, std::string name = {});

/// @brief Байт-код и карта «руна → узел графа, породивший её»: по номеру руны (`pc`) трасса исполнения подсвечивает узлы редактора.
struct CompiledGraph {
    Program program;
    std::vector<NodeId> source; ///< `source[pc]` — узел; размер равен числу рун программы.
};
/// @brief То же, что `compile`, с картой источников.
[[nodiscard]] std::expected<CompiledGraph, Diagnostic> compile_mapped(const Graph& graph, std::string name = {});
/// @brief Граф по байт-коду. Ошибка — программа не выражается графом (значение живёт на стеке между операторами).
[[nodiscard]] std::expected<Graph, Diagnostic> decompile(const Program& program);

[[nodiscard]] std::string serialize(const Graph& graph);
[[nodiscard]] std::expected<Graph, Diagnostic> parse_graph(std::string_view text);

} // namespace Runes
