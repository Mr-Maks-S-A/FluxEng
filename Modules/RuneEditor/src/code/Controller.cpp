#include <RuneEditor/Controller.hpp>

#include <algorithm>

namespace RuneEditor {

void ViewCamera::zoom_at(Vec2 screen, float factor) noexcept {
    const Vec2 before = to_world(screen);
    zoom = std::clamp(zoom * factor, 0.25f, 4.0f);
    pan = before - screen / zoom; // точка под курсором остаётся на месте
}

void ViewCamera::frame(const Rect& area, Vec2 viewport, float margin) noexcept {
    if (area.size.x <= 0.0f || area.size.y <= 0.0f) { pan = {0, 0}, zoom = 1.0f; return; }
    const float zx = (viewport.x - 2.0f * margin) / area.size.x, zy = (viewport.y - 2.0f * margin) / area.size.y;
    zoom = std::clamp(std::min(zx, zy), 0.25f, 1.5f);
    pan = area.center() - viewport / (2.0f * zoom);
}

void Controller::press(Button button, Vec2 screen, bool shift) {
    if (busy()) return;
    m_grab_screen = m_cursor = screen;
    m_grab_world = camera.to_world(screen);
    m_additive = shift;
    if (button == Button::Right) { m_mode = Mode::Pan; return; }

    const Pick hit = pick(m_editor->graph(), m_grab_world, camera.zoom);
    switch (hit.kind) {
    case Pick::Kind::Port: {
        const GraphNode& n = *m_editor->graph().find(hit.port.node);
        // Занятый вход — поднять ребро и нести от его источника.
        if (hit.port.kind == PortKind::Input && n.inputs[static_cast<std::size_t>(hit.port.index)] != Runes::no_node) {
            const NodeId from = n.inputs[static_cast<std::size_t>(hit.port.index)];
            m_editor->begin(), m_group_open = true;
            m_editor->disconnect({from, PortKind::Output, hit.port.node, hit.port.index});
            m_connecting = Connecting{{from, PortKind::Output, 0}, m_grab_world, false};
        } else {
            m_connecting = Connecting{hit.port, m_grab_world, false};
        }
        m_mode = Mode::Connect;
        break;
    }
    case Pick::Kind::Node:
        if (!m_editor->selection().contains(hit.node) || shift) m_editor->select(hit.node, shift);
        m_grab_positions.clear();
        for (const NodeId id : m_editor->selection()) {
            const GraphNode& n = *m_editor->graph().find(id);
            m_grab_positions[id] = {n.x, n.y};
        }
        m_mode = Mode::DragNodes;
        break;
    case Pick::Kind::Edge: {
        const EdgeRef e = hit.edge;
        m_editor->begin(), m_group_open = true;
        m_editor->disconnect(e);
        m_connecting = Connecting{{e.from, e.kind, 0}, m_grab_world, false};
        m_mode = Mode::Connect;
        break;
    }
    case Pick::Kind::None:
        if (!shift) m_editor->clear_selection();
        m_mode = Mode::Box;
        break;
    }
}

void Controller::move(Vec2 screen) {
    m_cursor = screen;
    const Vec2 world = camera.to_world(screen);
    switch (m_mode) {
    case Mode::Idle: m_hover = pick(m_editor->graph(), world, camera.zoom); break;
    case Mode::Pan: camera.pan = camera.pan - (screen - m_grab_screen) / camera.zoom, m_grab_screen = screen; break;
    case Mode::DragNodes:
        for (const auto& [id, origin] : m_grab_positions) m_editor->preview_move(id, origin + (world - m_grab_world));
        break;
    case Mode::Connect: {
        m_connecting->cursor = world;
        const Pick target = pick(m_editor->graph(), world, camera.zoom);
        m_connecting->valid = (target.kind == Pick::Kind::Port && m_editor->can_connect(m_connecting->from, target.port)) ||
                              (target.kind == Pick::Kind::Node && m_editor->can_connect(m_connecting->from, {target.node, PortKind::Input, 0}));
        break;
    }
    case Mode::Box: break;
    }
}

void Controller::finish_connect(Vec2 world) {
    const Pick target = pick(m_editor->graph(), world, camera.zoom);
    const PortRef from = m_connecting->from;
    if (target.kind == Pick::Kind::Port) m_editor->connect(from, target.port);
    else if (target.kind == Pick::Kind::Node) m_editor->connect(from, {target.node, PortKind::Input, 0});
    m_connecting.reset();
    if (m_group_open) m_editor->end(), m_group_open = false;
}

void Controller::release(Button button, Vec2 screen) {
    const Vec2 world = camera.to_world(screen);
    switch (m_mode) {
    case Mode::Idle: return;
    case Mode::Pan: if (button != Button::Right) return; break;
    case Mode::DragNodes: if (button != Button::Left) return; m_editor->commit_preview(), m_grab_positions.clear(); break;
    case Mode::Connect: if (button != Button::Left) return; finish_connect(world); break;
    case Mode::Box:
        if (button != Button::Left) return;
        if ((world - m_grab_world).length() * camera.zoom > 4.0f) m_editor->select_in(*box(), m_additive);
        break;
    }
    m_cursor = screen;
    m_mode = Mode::Idle;
    m_hover = pick(m_editor->graph(), world, camera.zoom);
}

void Controller::cancel() {
    if (m_mode == Mode::DragNodes) m_editor->cancel_preview(), m_grab_positions.clear();
    if (m_group_open) {
        m_editor->end(), m_group_open = false;
        m_editor->undo(); // вернуть поднятое ребро
    }
    m_connecting.reset();
    m_mode = Mode::Idle;
}

NodeId Controller::place(Runes::Rune rune, Vec2 screen, std::int32_t value) {
    const Vec2 at = camera.to_world(screen);
    const NodeId id = m_editor->add_node(rune, at, value);
    if (id != Runes::no_node) m_editor->select(id);
    return id;
}

std::optional<Rect> Controller::box() const noexcept {
    if (m_mode != Mode::Box) return std::nullopt;
    const Vec2 b = camera.to_world(m_cursor);
    const Vec2 lo{std::min(m_grab_world.x, b.x), std::min(m_grab_world.y, b.y)};
    const Vec2 hi{std::max(m_grab_world.x, b.x), std::max(m_grab_world.y, b.y)};
    return Rect{lo, hi - lo};
}

} // namespace RuneEditor
