#pragma once
/**
 * @file Geometry.hpp
 * @brief Геометрия графа рун на холсте: круглые «глифы» узлов, порты по кольцу, рёбра-кривые, выбор мышью.
 *
 * Узел — круг радиуса `node_radius`. Порты сидят на его кольце: **входы данных** — на левой дуге, **выход значения** и
 * **«следующий оператор»** — справа, **ветка JMP_IF** — снизу. Все размеры в единицах холста (при масштабе 1 — пиксели).
 * Модуль не знает, чем рисуют (его использует и отрисовка, и тесты): только числа.
 */

#include <Runes/Graph.hpp>

#include <cmath>
#include <optional>
#include <vector>

namespace RuneEditor {

using Runes::Graph;
using Runes::GraphNode;
using Runes::NodeId;

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    [[nodiscard]] friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    [[nodiscard]] friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    [[nodiscard]] friend constexpr Vec2 operator*(Vec2 a, float s) noexcept { return {a.x * s, a.y * s}; }
    [[nodiscard]] friend constexpr Vec2 operator/(Vec2 a, float s) noexcept { return {a.x / s, a.y / s}; }
    [[nodiscard]] friend constexpr bool operator==(Vec2, Vec2) noexcept = default;
    [[nodiscard]] float length() const noexcept { return std::sqrt(x * x + y * y); }
};

struct Rect {
    Vec2 position, size;
    [[nodiscard]] constexpr bool contains(Vec2 p) const noexcept {
        return p.x >= position.x && p.y >= position.y && p.x <= position.x + size.x && p.y <= position.y + size.y;
    }
    [[nodiscard]] constexpr Vec2 center() const noexcept { return position + size * 0.5f; }
    [[nodiscard]] constexpr bool intersects(const Rect& o) const noexcept {
        return position.x <= o.position.x + o.size.x && o.position.x <= position.x + size.x && position.y <= o.position.y + o.size.y && o.position.y <= position.y + size.y;
    }
};

inline constexpr float node_radius = 30.0f;
inline constexpr float port_radius = 7.0f;
inline constexpr float port_hit_radius = 12.0f;

/// @brief Вид порта: выход значения, вход данных (по номеру), «следующий оператор», ветка JMP_IF.
enum class PortKind : std::uint8_t { Output, Input, Next, Branch };

struct PortRef {
    NodeId node = Runes::no_node;
    PortKind kind = PortKind::Output;
    int index = 0; ///< Номер входа для `Input`.
    [[nodiscard]] friend constexpr bool operator==(const PortRef&, const PortRef&) noexcept = default;
};

/// @brief Ребро графа: откуда (узел и вид порта), куда (узел и номер входа для данных).
struct EdgeRef {
    NodeId from = Runes::no_node;
    PortKind kind = PortKind::Output; ///< Output — данные; Next / Branch — управление.
    NodeId to = Runes::no_node;
    int slot = 0;                     ///< Номер входа `to` (только для данных).
    [[nodiscard]] friend constexpr bool operator==(const EdgeRef&, const EdgeRef&) noexcept = default;
};

/// @brief Сколько входов данных у узла (по руне).
[[nodiscard]] int input_count(const GraphNode& node);
/// @brief Есть ли у узла такой порт (выход — у выражений; следующий — у операторов, кроме HALT; ветка — у JMP_IF).
[[nodiscard]] bool has_port(const GraphNode& node, PortKind kind, int index = 0);
/// @brief Положение порта на холсте; `nullopt`, если узла или порта нет.
[[nodiscard]] std::optional<Vec2> port_position(const Graph& graph, const PortRef& port);
/// @brief Все порты узла (для рисования).
[[nodiscard]] std::vector<PortRef> ports_of(NodeId id, const GraphNode& node);
/// @brief Все рёбра графа: данные (`inputs`) и управление (`next`, `branch`).
[[nodiscard]] std::vector<EdgeRef> edges_of(const Graph& graph);
/// @brief Ломаная кривой Безье ребра между двумя точками (для рисования и выбора ребра).
[[nodiscard]] std::vector<Vec2> edge_polyline(Vec2 from, Vec2 to, int segments = 20);
/// @brief Концы ребра на холсте; `nullopt`, если узлов нет.
[[nodiscard]] std::optional<std::pair<Vec2, Vec2>> edge_endpoints(const Graph& graph, const EdgeRef& edge);

/// @brief Что под курсором.
struct Pick {
    enum class Kind : std::uint8_t { None, Port, Node, Edge };
    Kind kind = Kind::None;
    NodeId node = Runes::no_node; ///< Для Node и Port.
    PortRef port;                 ///< Для Port.
    EdgeRef edge;                 ///< Для Edge.
};

/**
 * @brief Выбор под курсором: сначала порты, затем узлы (верхний — с большим номером), затем рёбра.
 * @param zoom Масштаб холста: радиусы попадания держатся постоянными на экране.
 */
[[nodiscard]] Pick pick(const Graph& graph, Vec2 world, float zoom = 1.0f);
/// @brief Прямоугольник, охватывающий узлы (с запасом); пустой граф — нулевой.
[[nodiscard]] Rect bounds_of(const Graph& graph);

} // namespace RuneEditor
