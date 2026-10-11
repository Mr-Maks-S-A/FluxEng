#include <InputSystem/ActionMap.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <stdexcept>

namespace InputSystem {

namespace {

constexpr float kDefaultDeadzone = 0.15f;

bool valid_action_name(std::string_view name) noexcept {
    if (name.empty()) return false;
    return std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '-'; });
}

float apply_deadzone(float raw, const Binding& b) noexcept {
    const float magnitude = std::fabs(raw);
    if (magnitude < b.deadzone) return 0.0f;
    const float range = 1.0f - b.deadzone;
    const float scaled = range > 1e-6f ? (magnitude - b.deadzone) / range : 1.0f;
    return std::copysign(std::fmin(scaled, 1.0f), raw) * b.scale;
}

/// Ось с наибольшим по модулю значением среди подключённых геймпадов (сырое и «на начало кадра»).
float pad_axis(const InputState& s, GamepadAxis axis, bool previous) noexcept {
    float best = 0.0f;
    for (std::size_t pad = 0; pad < kMaxGamepads; ++pad) {
        if (!s.gamepad_connected(pad)) continue;
        const float v = previous ? s.gamepad_axis_previous(pad, axis) : s.gamepad_axis(pad, axis);
        if (std::fabs(v) > std::fabs(best)) best = v;
    }
    return best;
}

bool mods_ok(const Binding& b, const InputState& s) noexcept { return b.required == Modifiers::None || has_all(s.modifiers(), b.required); }

bool any_pad(const InputState& s, GamepadButton button, bool (InputState::*query)(std::size_t, GamepadButton) const noexcept) noexcept {
    for (std::size_t pad = 0; pad < kMaxGamepads; ++pad) {
        if ((s.*query)(pad, button)) return true;
    }
    return false;
}

float binding_value(const Binding& b, const InputState& s) noexcept {
    switch (b.source) {
        case Binding::Source::Key: return s.down(static_cast<Key>(b.code)) && mods_ok(b, s) ? 1.0f : 0.0f;
        case Binding::Source::Mouse: return s.mouse_down(static_cast<MouseButton>(b.code)) && mods_ok(b, s) ? 1.0f : 0.0f;
        case Binding::Source::PadButton: return any_pad(s, static_cast<GamepadButton>(b.code), &InputState::gamepad_down) ? 1.0f : 0.0f;
        case Binding::Source::PadAxis: return apply_deadzone(pad_axis(s, static_cast<GamepadAxis>(b.code), false), b);
        case Binding::Source::KeyAxis:
            return (s.down(static_cast<Key>(b.code2)) ? 1.0f : 0.0f) - (s.down(static_cast<Key>(b.code)) ? 1.0f : 0.0f);
    }
    return 0.0f;
}

bool binding_down(const Binding& b, const InputState& s) noexcept {
    const float v = binding_value(b, s);
    return b.source == Binding::Source::PadAxis ? std::fabs(v) >= ActionMap::kAxisPressThreshold : v != 0.0f;
}

bool axis_crossed(const Binding& b, const InputState& s, bool upward) noexcept {
    const auto axis = static_cast<GamepadAxis>(b.code);
    const float now = std::fabs(apply_deadzone(pad_axis(s, axis, false), b));
    const float before = std::fabs(apply_deadzone(pad_axis(s, axis, true), b));
    const float threshold = ActionMap::kAxisPressThreshold;
    return upward ? (now >= threshold && before < threshold) : (now < threshold && before >= threshold);
}

bool binding_pressed(const Binding& b, const InputState& s) noexcept {
    switch (b.source) {
        case Binding::Source::Key: return s.pressed(static_cast<Key>(b.code)) && mods_ok(b, s);
        case Binding::Source::Mouse: return s.mouse_pressed(static_cast<MouseButton>(b.code)) && mods_ok(b, s);
        case Binding::Source::PadButton: return any_pad(s, static_cast<GamepadButton>(b.code), &InputState::gamepad_pressed);
        case Binding::Source::PadAxis: return axis_crossed(b, s, true);
        case Binding::Source::KeyAxis: return s.pressed(static_cast<Key>(b.code)) || s.pressed(static_cast<Key>(b.code2));
    }
    return false;
}

bool binding_released(const Binding& b, const InputState& s) noexcept {
    switch (b.source) {
        case Binding::Source::Key: return s.released(static_cast<Key>(b.code));
        case Binding::Source::Mouse: return s.mouse_released(static_cast<MouseButton>(b.code));
        case Binding::Source::PadButton: return any_pad(s, static_cast<GamepadButton>(b.code), &InputState::gamepad_released);
        case Binding::Source::PadAxis: return axis_crossed(b, s, false);
        case Binding::Source::KeyAxis: return s.released(static_cast<Key>(b.code)) || s.released(static_cast<Key>(b.code2));
    }
    return false;
}

std::string mods_prefix(Modifiers mods) {
    std::string out;
    if (has_all(mods, Modifiers::Control)) out += "Ctrl+";
    if (has_all(mods, Modifiers::Shift)) out += "Shift+";
    if (has_all(mods, Modifiers::Alt)) out += "Alt+";
    if (has_all(mods, Modifiers::Super)) out += "Super+";
    return out;
}

} // namespace

// ===================================================================== привязки

ActionMap::Action& ActionMap::get_or_add(std::string_view name) {
    if (!valid_action_name(name)) throw std::invalid_argument(std::format("ActionMap: bad action name '{}' (letters, digits, '_', '.', '-')", name));
    const ActionId id = action_id(name);
    if (const Action* existing = find(id)) {
        if (existing->name != name) throw std::invalid_argument(std::format("ActionMap: actions '{}' and '{}' have the same id", existing->name, name));
        return const_cast<Action&>(*existing); // NOLINT: find() const, объект наш и не const
    }
    m_actions.push_back(Action{id, std::string(name), {}});
    rebuild_index();
    return m_actions.back();
}

void ActionMap::rebuild_index() {
    m_index.clear();
    m_index.reserve(m_actions.size());
    for (std::size_t i = 0; i < m_actions.size(); ++i) m_index.emplace_back(m_actions[i].id.value, static_cast<std::uint32_t>(i));
    std::ranges::sort(m_index);
}

const ActionMap::Action* ActionMap::find(ActionId id) const noexcept {
    const auto it = std::ranges::lower_bound(m_index, id.value, {}, &std::pair<std::uint32_t, std::uint32_t>::first);
    return it != m_index.end() && it->first == id.value ? &m_actions[it->second] : nullptr;
}

ActionMap& ActionMap::bind(std::string_view action, Key key, Modifiers required) {
    get_or_add(action).bindings.push_back(Binding{.source = Binding::Source::Key, .code = static_cast<std::uint16_t>(key), .required = required});
    return *this;
}
ActionMap& ActionMap::bind(std::string_view action, MouseButton button, Modifiers required) {
    get_or_add(action).bindings.push_back(Binding{.source = Binding::Source::Mouse, .code = static_cast<std::uint16_t>(button), .required = required});
    return *this;
}
ActionMap& ActionMap::bind(std::string_view action, GamepadButton button) {
    get_or_add(action).bindings.push_back(Binding{.source = Binding::Source::PadButton, .code = static_cast<std::uint16_t>(button)});
    return *this;
}
ActionMap& ActionMap::bind_axis(std::string_view action, GamepadAxis axis, float scale, float deadzone) {
    get_or_add(action).bindings.push_back(
        Binding{.source = Binding::Source::PadAxis, .code = static_cast<std::uint16_t>(axis), .scale = scale, .deadzone = std::clamp(deadzone, 0.0f, 0.99f)});
    return *this;
}
ActionMap& ActionMap::bind_keys(std::string_view action, Key negative, Key positive) {
    get_or_add(action).bindings.push_back(
        Binding{.source = Binding::Source::KeyAxis, .code = static_cast<std::uint16_t>(negative), .code2 = static_cast<std::uint16_t>(positive)});
    return *this;
}

bool ActionMap::unbind(std::string_view action) {
    const ActionId id = action_id(action);
    const bool removed = std::erase_if(m_actions, [id](const Action& a) { return a.id == id; }) > 0;
    if (removed) rebuild_index();
    return removed;
}

// ===================================================================== запросы

std::vector<std::string_view> ActionMap::names() const {
    std::vector<std::string_view> out;
    out.reserve(m_actions.size());
    for (const Action& action : m_actions) out.push_back(action.name);
    return out;
}

std::span<const Binding> ActionMap::bindings(ActionId id) const noexcept {
    const Action* action = find(id);
    return action != nullptr ? std::span<const Binding>(action->bindings) : std::span<const Binding>();
}

bool ActionMap::down(ActionId id, const InputState& state) const noexcept {
    return std::ranges::any_of(bindings(id), [&](const Binding& b) { return binding_down(b, state); });
}
bool ActionMap::pressed(ActionId id, const InputState& state) const noexcept {
    return std::ranges::any_of(bindings(id), [&](const Binding& b) { return binding_pressed(b, state); });
}
bool ActionMap::released(ActionId id, const InputState& state) const noexcept {
    return std::ranges::any_of(bindings(id), [&](const Binding& b) { return binding_released(b, state); });
}

float ActionMap::value(ActionId id, const InputState& state) const noexcept {
    float sum = 0.0f;
    for (const Binding& b : bindings(id)) sum += binding_value(b, state);
    return std::fmax(-1.0f, std::fmin(1.0f, sum));
}

Vec2f ActionMap::vec2(ActionId x, ActionId y, const InputState& state) const noexcept {
    Vec2f v{value(x, state), value(y, state)};
    const float length = std::sqrt(v.x * v.x + v.y * v.y);
    if (length > 1.0f) {
        v.x /= length;
        v.y /= length;
    }
    return v;
}

// ===================================================================== текст

std::string ActionMap::to_text() const {
    std::string out;
    for (const Action& action : m_actions) {
        out += action.name;
        out += ':';
        for (const Binding& b : action.bindings) {
            out += ' ';
            switch (b.source) {
                case Binding::Source::Key: out += std::format("{}Key:{}", mods_prefix(b.required), to_string(static_cast<Key>(b.code))); break;
                case Binding::Source::Mouse: out += std::format("{}Mouse:{}", mods_prefix(b.required), to_string(static_cast<MouseButton>(b.code))); break;
                case Binding::Source::PadButton: out += std::format("Pad:{}", to_string(static_cast<GamepadButton>(b.code))); break;
                case Binding::Source::KeyAxis:
                    out += std::format("Keys:{},{}", to_string(static_cast<Key>(b.code)), to_string(static_cast<Key>(b.code2)));
                    break;
                case Binding::Source::PadAxis:
                    out += std::format("Axis:{}", to_string(static_cast<GamepadAxis>(b.code)));
                    if (b.scale != 1.0f) out += std::format("*{}", b.scale);
                    if (b.deadzone != kDefaultDeadzone) out += std::format("~{}", b.deadzone);
                    break;
            }
        }
        out += '\n';
    }
    return out;
}

namespace {

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::optional<float> parse_float(std::string_view text) noexcept {
    float value = 0.0f;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

} // namespace

std::expected<ActionMap, std::string> ActionMap::from_text(std::string_view text) {
    ActionMap map;
    std::size_t line_no = 0;
    const auto error = [&](std::string message) { return std::unexpected(std::format("line {}: {}", line_no, std::move(message))); };

    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        ++line_no;
        if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) return error("expected 'action: bindings'");
        const std::string_view name = trim(line.substr(0, colon));
        if (!valid_action_name(name)) return error(std::format("bad action name '{}'", name));
        if (map.has(name)) return error(std::format("action '{}' is defined twice", name));
        Action& action = map.get_or_add(name);

        std::string_view rest = trim(line.substr(colon + 1));
        while (!rest.empty()) {
            const std::size_t space = rest.find_first_of(" \t");
            std::string_view token = rest.substr(0, space);
            rest = space == std::string_view::npos ? std::string_view{} : trim(rest.substr(space));

            Modifiers mods = Modifiers::None;
            for (;;) { // префиксы Ctrl+ Shift+ Alt+ Super+
                const std::size_t plus = token.find('+');
                if (plus == std::string_view::npos || token.find(':') < plus) break;
                const std::string_view mod = token.substr(0, plus);
                if (mod == "Ctrl") mods |= Modifiers::Control;
                else if (mod == "Shift") mods |= Modifiers::Shift;
                else if (mod == "Alt") mods |= Modifiers::Alt;
                else if (mod == "Super") mods |= Modifiers::Super;
                else return error(std::format("unknown modifier '{}'", mod));
                token.remove_prefix(plus + 1);
            }
            const std::size_t sep = token.find(':');
            if (sep == std::string_view::npos) return error(std::format("bad binding '{}' (expected Source:Name)", token));
            const std::string_view source = token.substr(0, sep);
            std::string_view arg = token.substr(sep + 1);

            if (source == "Key") {
                const auto key = parse_key(arg);
                if (!key) return error(std::format("unknown key '{}'", arg));
                action.bindings.push_back(Binding{.source = Binding::Source::Key, .code = static_cast<std::uint16_t>(*key), .required = mods});
            } else if (source == "Mouse") {
                const auto button = parse_mouse_button(arg);
                if (!button) return error(std::format("unknown mouse button '{}'", arg));
                action.bindings.push_back(Binding{.source = Binding::Source::Mouse, .code = static_cast<std::uint16_t>(*button), .required = mods});
            } else if (source == "Pad") {
                const auto button = parse_gamepad_button(arg);
                if (!button) return error(std::format("unknown gamepad button '{}'", arg));
                action.bindings.push_back(Binding{.source = Binding::Source::PadButton, .code = static_cast<std::uint16_t>(*button)});
            } else if (source == "Keys") {
                const std::size_t comma = arg.find(',');
                if (comma == std::string_view::npos) return error("Keys needs two keys: Keys:Minus,Plus");
                const auto negative = parse_key(arg.substr(0, comma));
                const auto positive = parse_key(arg.substr(comma + 1));
                if (!negative || !positive) return error(std::format("unknown key in '{}'", arg));
                action.bindings.push_back(Binding{.source = Binding::Source::KeyAxis, .code = static_cast<std::uint16_t>(*negative), .code2 = static_cast<std::uint16_t>(*positive)});
            } else if (source == "Axis") {
                float scale = 1.0f, deadzone = kDefaultDeadzone;
                if (const auto tilde = arg.find('~'); tilde != std::string_view::npos) {
                    const auto value = parse_float(arg.substr(tilde + 1));
                    if (!value || *value < 0.0f || *value >= 1.0f) return error("bad dead zone (0…<1) after '~'");
                    deadzone = *value;
                    arg = arg.substr(0, tilde);
                }
                if (const auto star = arg.find('*'); star != std::string_view::npos) {
                    const auto value = parse_float(arg.substr(star + 1));
                    if (!value) return error("bad scale after '*'");
                    scale = *value;
                    arg = arg.substr(0, star);
                }
                const auto axis = parse_gamepad_axis(arg);
                if (!axis) return error(std::format("unknown gamepad axis '{}'", arg));
                action.bindings.push_back(Binding{.source = Binding::Source::PadAxis, .code = static_cast<std::uint16_t>(*axis), .scale = scale, .deadzone = deadzone});
            } else {
                return error(std::format("unknown source '{}' (Key, Mouse, Pad, Keys, Axis)", source));
            }
        }
    }
    return map;
}

} // namespace InputSystem
