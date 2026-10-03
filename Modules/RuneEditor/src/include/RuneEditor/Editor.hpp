#pragma once
/**
 * @file Editor.hpp
 * @brief Модель редактора графа рун: каждая правка — `Op` (данные), история undo/redo, выбор, перетаскивание без шума.
 *
 * ```
 *   мышь/клавиши ──Controller──► Op ──┐
 *   автотест / повтор / автосохранение ┴─► Editor::execute(Op) ──► Graph
 *                                              │ observer(Op)  → журнал (Autosave), сеть, подсветка
 * ```
 * Единая точка `execute` — тот же приём, что у команд симуляции: правки можно записать, воспроизвести и (когда
 * придёт мультиплеер) отправить по сети. `Op` — простая структура (тривиально копируемая), как событие EventLog.
 *
 * История — снимки графа (граф мал: десятки узлов): надёжнее обратных операций. Группа правок между `Begin` и `End` —
 * один шаг истории. Перетаскивание узлов (`preview_move`) не попадает ни в историю, ни в журнал, пока не зафиксировано
 * `commit_preview` — тогда это одна группа `MoveNode`.
 */

#include <RuneEditor/Geometry.hpp>

#include <functional>
#include <map>
#include <set>

namespace RuneEditor {

/// @brief Одна правка графа. Поля, не нужные виду правки, игнорируются (см. комментарий у вида).
struct Op {
    enum class Kind : std::uint8_t {
        AddNode,   ///< rune, value, x, y → новый узел (его номер — в `Result::node`).
        RemoveNode,///< node.
        MoveNode,  ///< node, x, y.
        SetValue,  ///< node, value (PUSH).
        SetInput,  ///< node, slot, other (no_node — отключить).
        SetNext,   ///< node, other.
        SetBranch, ///< node, other.
        SetEntry,  ///< node.
        Begin,     ///< Начало группы.
        End,       ///< Конец группы.
        Undo,
        Redo,
    };
    static constexpr std::string_view event_name = "rune_editor.op";

    Kind kind = Kind::Begin;
    Runes::Rune rune = Runes::Rune::Halt;
    std::uint8_t reserved[2] = {};
    std::int32_t slot = 0;
    NodeId node = Runes::no_node;
    NodeId other = Runes::no_node;
    std::int32_t value = 0;
    float x = 0.0f, y = 0.0f;
};
static_assert(std::is_trivially_copyable_v<Op>);

struct OpResult {
    bool ok = false;
    NodeId node = Runes::no_node; ///< Для AddNode — новый узел.
};

class Editor {
public:
    explicit Editor(Graph graph = {}) : m_graph(std::move(graph)) {}

    [[nodiscard]] const Graph& graph() const noexcept { return m_graph; }
    /// @brief Заменяет граф целиком (загрузка файла): очищает историю и выбор; наблюдатель не уведомляется.
    void reset(Graph graph);

    /// @brief Единственная точка изменения графа. Недопустимая правка (нет узла, цикл, неверная ширина) — `ok = false`, граф и история не меняются.
    OpResult execute(const Op& op);

    // --- удобные обёртки над execute
    NodeId add_node(Runes::Rune rune, Vec2 at, std::int32_t value = 0);
    bool remove_node(NodeId id);
    bool move_node(NodeId id, Vec2 to);
    bool set_value(NodeId id, std::int32_t value);
    bool set_entry(NodeId id);
    bool disconnect(const EdgeRef& edge);
    void begin();
    void end();
    bool undo() { return execute({.kind = Op::Kind::Undo}).ok; }
    bool redo() { return execute({.kind = Op::Kind::Redo}).ok; }
    [[nodiscard]] bool can_undo() const noexcept { return !m_undo.empty() && m_depth == 0; }
    [[nodiscard]] bool can_redo() const noexcept { return !m_redo.empty() && m_depth == 0; }

    /// @brief Можно ли соединить порты. Данные: выход → вход (ширина совпадает, нет цикла). Управление: Next/Branch → любой оператор.
    /// Порты можно давать в любом порядке (вход, потом выход).
    [[nodiscard]] bool can_connect(const PortRef& a, const PortRef& b) const;
    /// @brief Соединяет (одной правкой); для Next/Branch цель — `b.node`. Возвращает, получилось ли.
    bool connect(const PortRef& a, const PortRef& b);

    // --- перетаскивание без шума: до фиксации граф двигается «вживую», история и журнал молчат
    void preview_move(NodeId id, Vec2 to);
    void commit_preview();
    void cancel_preview();
    [[nodiscard]] bool previewing() const noexcept { return !m_origin.empty(); }

    // --- выбор (не правка: в историю не идёт)
    [[nodiscard]] const std::set<NodeId>& selection() const noexcept { return m_selection; }
    void select(NodeId id, bool additive = false);
    void select_in(const Rect& rect, bool additive = false);
    void select_all();
    void clear_selection() { m_selection.clear(); }
    /// @brief Удаляет выбранные узлы (одна группа).
    void erase_selection();

    /// @brief Получает каждую выполненную правку (включая Begin/End/Undo/Redo). Один наблюдатель.
    void set_observer(std::function<void(const Op&)> observer) { m_observer = std::move(observer); }

    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; } ///< Растёт при любом изменении графа (для перекомпиляции по требованию).

private:
    void push_history();
    void changed() { ++m_revision; }

    Graph m_graph;
    std::vector<Graph> m_undo, m_redo;
    int m_depth = 0;
    bool m_group_pushed = false;
    std::map<NodeId, Vec2> m_origin;
    std::set<NodeId> m_selection;
    std::function<void(const Op&)> m_observer;
    std::uint64_t m_revision = 0;
};

} // namespace RuneEditor
