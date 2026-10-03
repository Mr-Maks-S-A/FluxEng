#pragma once
/**
 * @file Analysis.hpp
 * @brief Живая диагностика графа: компиляция с картой «руна → узел», проблемы по узлам, оценка стоимости.
 *
 * Редактор зовёт `analyze` после каждой правки (`Editor::revision` изменился). Компилятор сообщает лишь первую ошибку,
 * поэтому анализ добавляет свои проверки: у каждого узла — своя проблема, видимая на холсте (красный/жёлтый ободок).
 */

#include <RuneEditor/Geometry.hpp>

#include <Runes/Diagnostic.hpp>
#include <Runes/Spells.hpp>

#include <optional>

namespace RuneEditor {

struct NodeProblem {
    NodeId node = Runes::no_node;
    Runes::Code code = Runes::Code::ProgramEmpty;
    bool error = true;     ///< `true` — граф из-за этого не соберётся; `false` — предупреждение.
    std::string detail;
};

struct Analysis {
    std::optional<Runes::Program> program;       ///< Есть, если граф компилируется.
    std::vector<NodeId> source;                  ///< `source[pc]` — узел, породивший руну `pc`.
    std::optional<Runes::Diagnostic> error;      ///< Ошибка компилятора, если не собралось.
    std::vector<NodeProblem> problems;           ///< Ошибки и предупреждения по узлам.
    Runes::CostEstimate cost;                    ///< Для собравшегося графа.

    [[nodiscard]] bool ok() const noexcept { return program.has_value(); }
    /// @brief Проблемы узла (для подсказки при наведении).
    [[nodiscard]] std::vector<const NodeProblem*> of(NodeId node) const;
    /// @brief Самая тяжёлая проблема узла: ошибка важнее предупреждения; `nullptr` — чисто.
    [[nodiscard]] const NodeProblem* worst(NodeId node) const;
    /// @brief Узел → индекс руны (первой, порождённой узлом); `nullopt` — узел не вошёл в программу. Для подсветки трассы наоборот: `source[pc]`.
    [[nodiscard]] std::optional<std::size_t> first_rune_of(NodeId node) const;
};

[[nodiscard]] Analysis analyze(const Graph& graph, const Runes::Tuning& tuning = {}, std::string name = {});

} // namespace RuneEditor
