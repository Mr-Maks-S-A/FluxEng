#include <RuneEditor/View.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace RuneEditor {

using RendererSystem::Color;
using Runes::Rune;

Family family_of(Rune rune) noexcept {
    switch (rune) {
    case Rune::Push: case Rune::Dup: case Rune::Drop: return Family::Data;
    case Rune::Add: case Rune::Mul: return Family::Arithmetic;
    case Rune::Caster: case Rune::Aim: case Rune::Target: return Family::Context;
    case Rune::ManaAt: return Family::Sense;
    case Rune::JmpIf: case Rune::Halt: return Family::Control;
    default: return Family::Effect;
    }
}

Color family_color(Family family) noexcept {
    switch (family) {
    case Family::Data: return {120, 130, 150, 255};
    case Family::Arithmetic: return {110, 150, 220, 255};
    case Family::Context: return {90, 190, 150, 255};
    case Family::Sense: return {80, 160, 230, 255};
    case Family::Control: return {210, 170, 80, 255};
    case Family::Effect: return {220, 100, 110, 255};
    }
    return {255, 255, 255, 255};
}

std::string glyph_label(const GraphNode& node) {
    const std::string name(Runes::rune_name(node.rune));
    if (node.rune != Rune::Push) return name;
    return std::format("{}\n{:g}", name, static_cast<double>(node.value) / 65536.0);
}

View::View(RendererSystem::Renderer2D& renderer)
    : m_disc(renderer.create_texture(RendererSystem::Procedural::circle_image(96, RendererSystem::Colors::white), {.filter = RendererSystem::TextureFilter::Linear})),
      m_ring(renderer.create_texture(RendererSystem::Procedural::circle_image(96, RendererSystem::Colors::white, 5.0f), {.filter = RendererSystem::TextureFilter::Linear})) {}

namespace {
Color with_alpha(Color c, std::uint8_t a) { return {c.r, c.g, c.b, a}; }
Color darker(Color c, float k) { return {static_cast<std::uint8_t>(c.r * k), static_cast<std::uint8_t>(c.g * k), static_cast<std::uint8_t>(c.b * k), c.a}; }
} // namespace

void View::draw(RendererSystem::Renderer2D& r, RendererSystem::FontHandle font, const Editor& editor, const Controller& controller, const Marks& marks,
                const RendererSystem::Rect& area, std::int32_t layer) const {
    const Graph& graph = editor.graph();
    const ViewCamera& cam = controller.camera;
    const glm::vec2 origin = area.position;
    const float zoom = cam.zoom;
    const auto at = [&](Vec2 world) { const Vec2 s = cam.to_screen(world); return glm::vec2{origin.x + s.x, origin.y + s.y}; };

    r.fill_rect(area, Color{8, 10, 20, 215}, layer);

    // Сетка холста: тоньше при отдалении.
    const float step = 50.0f * zoom;
    if (step > 12.0f) {
        const Vec2 anchor = cam.to_screen({std::floor(cam.pan.x / 50.0f) * 50.0f, std::floor(cam.pan.y / 50.0f) * 50.0f});
        for (float x = anchor.x; x < area.size.x; x += step) r.fill_rect({{origin.x + x, origin.y}, {1.0f, area.size.y}}, Color{255, 255, 255, 14}, layer + 1);
        for (float y = anchor.y; y < area.size.y; y += step) r.fill_rect({{origin.x, origin.y + y}, {area.size.x, 1.0f}}, Color{255, 255, 255, 14}, layer + 1);
    }

    // Рёбра.
    const Pick& hover = controller.hover();
    for (const EdgeRef& e : edges_of(graph)) {
        const auto ends = edge_endpoints(graph, e);
        if (!ends) continue;
        const bool data = e.kind == PortKind::Output;
        Color color = data ? Color{110, 190, 240, 255} : Color{240, 200, 90, 255};
        if (hover.kind == Pick::Kind::Edge && hover.edge == e) color = Color{255, 255, 255, 255};
        const auto line = edge_polyline(ends->first, ends->second);
        for (std::size_t i = 0; i + 1 < line.size(); ++i) r.draw_line(at(line[i]), at(line[i + 1]), (data ? 2.0f : 3.0f) * std::max(zoom, 0.6f), color, layer + 2);
    }
    if (const auto& c = controller.connecting()) {
        const auto from = port_position(graph, c->from);
        if (from) {
            const bool out_first = c->from.kind != PortKind::Input;
            const auto line = out_first ? edge_polyline(*from, c->cursor) : edge_polyline(c->cursor, *from);
            const Color color = c->valid ? Color{120, 255, 160, 255} : Color{255, 255, 255, 140};
            for (std::size_t i = 0; i + 1 < line.size(); ++i) r.draw_line(at(line[i]), at(line[i + 1]), 2.5f, color, layer + 6);
        }
    }

    // Узлы.
    for (const auto& [id, node] : graph.nodes()) {
        const glm::vec2 c = at({node.x, node.y});
        const float radius = node_radius * zoom;
        if (c.x + radius < origin.x || c.y + radius < origin.y || c.x - radius > origin.x + area.size.x || c.y - radius > origin.y + area.size.y) continue;
        const Color base = family_color(family_of(node.rune));
        const float glow = marks.activity && marks.activity->contains(id) ? marks.activity->at(id) : 0.0f;
        if (glow > 0.01f) { // свечение исполнения — широкое мягкое кольцо
            const float gr = radius * (1.5f + 0.3f * glow);
            r.draw({.position = c, .size = {gr * 2, gr * 2}, .color = Color{120, 255, 150, static_cast<std::uint8_t>(150.0f * glow)}, .texture = m_disc, .layer = layer + 3});
        }
        r.draw({.position = c, .size = {radius * 2, radius * 2}, .color = darker(base, 0.42f), .texture = m_disc, .layer = layer + 4});
        Color ring = base;
        float ring_scale = 1.0f;
        if (const NodeProblem* p = marks.analysis ? marks.analysis->worst(id) : nullptr) ring = p->error ? Color{255, 80, 80, 255} : Color{255, 210, 70, 255}, ring_scale = 1.12f;
        if (editor.selection().contains(id)) ring = Color{255, 255, 255, 255}, ring_scale = 1.12f;
        r.draw({.position = c, .size = {radius * 2 * ring_scale, radius * 2 * ring_scale}, .color = ring, .texture = m_ring, .layer = layer + 5});
        if (graph.entry == id) r.draw({.position = c, .size = {radius * 2.5f, radius * 2.5f}, .color = Color{240, 200, 90, 220}, .texture = m_ring, .layer = layer + 5});

        const std::string label = glyph_label(node);
        const float size = std::max(10.0f, 13.0f * zoom);
        const glm::vec2 text_size = r.measure_text(font, label, {.size = size});
        r.draw_text(font, label, c - text_size * 0.5f, {.size = size, .layer = layer + 7, .shadow = Color{0, 0, 0, 200}});

        // Порты: кружки на кольце; под курсором — крупнее.
        for (const PortRef& port : ports_of(id, node)) {
            const glm::vec2 p = at(*port_position(graph, port));
            const bool hot = hover.kind == Pick::Kind::Port && hover.port == port;
            const bool taken = port.kind == PortKind::Input && node.inputs[static_cast<std::size_t>(port.index)] != Runes::no_node;
            const float pr = port_radius * std::max(zoom, 0.7f) * (hot ? 1.4f : 1.0f);
            const Color pc = port.kind == PortKind::Output || port.kind == PortKind::Input ? Color{110, 190, 240, 255} : Color{240, 200, 90, 255};
            r.draw({.position = p, .size = {pr * 2, pr * 2}, .color = taken ? pc : with_alpha(pc, 120), .texture = m_disc, .layer = layer + 6});
        }
    }

    if (const auto box = controller.box()) {
        const glm::vec2 a = at(box->position), b = at(box->position + box->size);
        r.fill_rect({a, b - a}, Color{120, 170, 255, 40}, layer + 8);
        r.draw_rect({a, b - a}, 1.0f, Color{160, 200, 255, 200}, layer + 8);
    }

    // Подсказка у курсора: проблемы узла под курсором.
    if (marks.analysis && hover.kind == Pick::Kind::Node) {
        std::string text;
        for (const NodeProblem* p : marks.analysis->of(hover.node)) {
            const Runes::Diagnostic d{p->code, 0, p->node, p->detail};
            text += (p->error ? "[X] " : "[!] ") + d.message() + "\n";
        }
        if (!text.empty()) {
            text.pop_back();
            const GraphNode& n = *graph.find(hover.node);
            const glm::vec2 tip = at({n.x + node_radius + 6.0f, n.y - node_radius});
            const glm::vec2 size = r.measure_text(font, text, {.size = 14.0f});
            r.fill_rect({tip - 4.0f, size + 8.0f}, Color{0, 0, 0, 220}, layer + 9);
            r.draw_text(font, text, tip, {.size = 14.0f, .color = Color{255, 220, 160, 255}, .layer = layer + 10});
        }
    }

    // Строка состояния редактора: цена заклинания, число рун, замок.
    std::string status = std::format("{} узл. | {} | правок: undo {} redo {}", graph.nodes().size(), marks.locked ? "ТОЛЬКО ЧТЕНИЕ (запись/повтор)" : "правка",
                                     editor.can_undo() ? "да" : "—", editor.can_redo() ? "да" : "—");
    if (marks.analysis) {
        const Analysis& a = *marks.analysis;
        if (a.ok()) {
            status += std::format(" | {} рун, ~{:.1f} маны за проход{}{}", a.cost.runes, a.cost.per_pass.to_double(), a.cost.exact ? "" : " (радиус неизвестен)", a.cost.loops ? ", есть цикл" : "");
        } else if (a.error) {
            status += " | [X] " + a.error->format();
        }
    }
    r.fill_rect({{origin.x, origin.y + area.size.y - 26.0f}, {area.size.x, 26.0f}}, Color{0, 0, 0, 200}, layer + 9);
    r.draw_text(font, status, {origin.x + 8.0f, origin.y + area.size.y - 22.0f}, {.size = 15.0f, .color = marks.analysis && !marks.analysis->ok() ? Color{255, 130, 130, 255} : Color{200, 230, 255, 255}, .layer = layer + 10});
}

} // namespace RuneEditor
