#include <Core/Actions.hpp>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>

namespace Core {

namespace {

struct Named {
    std::string_view name;
    int code;
};

constexpr std::array<Named, 28> special_keys = {{
    {"SPACE", GLFW_KEY_SPACE},           {"TAB", GLFW_KEY_TAB},
    {"ENTER", GLFW_KEY_ENTER},           {"ESCAPE", GLFW_KEY_ESCAPE},
    {"BACKSPACE", GLFW_KEY_BACKSPACE},   {"UP", GLFW_KEY_UP},
    {"DOWN", GLFW_KEY_DOWN},             {"LEFT", GLFW_KEY_LEFT},
    {"RIGHT", GLFW_KEY_RIGHT},           {"LEFT_SHIFT", GLFW_KEY_LEFT_SHIFT},
    {"RIGHT_SHIFT", GLFW_KEY_RIGHT_SHIFT}, {"LEFT_CONTROL", GLFW_KEY_LEFT_CONTROL},
    {"RIGHT_CONTROL", GLFW_KEY_RIGHT_CONTROL}, {"LEFT_ALT", GLFW_KEY_LEFT_ALT},
    {"RIGHT_ALT", GLFW_KEY_RIGHT_ALT},   {"MINUS", GLFW_KEY_MINUS},
    {"EQUAL", GLFW_KEY_EQUAL},           {"COMMA", GLFW_KEY_COMMA},
    {"PERIOD", GLFW_KEY_PERIOD},         {"SLASH", GLFW_KEY_SLASH},
    {"GRAVE", GLFW_KEY_GRAVE_ACCENT},    {"HOME", GLFW_KEY_HOME},
    {"END", GLFW_KEY_END},               {"PAGE_UP", GLFW_KEY_PAGE_UP},
    {"PAGE_DOWN", GLFW_KEY_PAGE_DOWN},   {"INSERT", GLFW_KEY_INSERT},
    {"DELETE", GLFW_KEY_DELETE},         {"CAPS_LOCK", GLFW_KEY_CAPS_LOCK},
}};

constexpr std::array<Named, 3> mouse_buttons = {{
    {"MOUSE_LEFT", GLFW_MOUSE_BUTTON_LEFT},
    {"MOUSE_RIGHT", GLFW_MOUSE_BUTTON_RIGHT},
    {"MOUSE_MIDDLE", GLFW_MOUSE_BUTTON_MIDDLE},
}};

std::string upper(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

} // namespace

std::string binding_name(Binding b) {
    if (b.device == Binding::Device::Mouse) {
        for (const Named& m : mouse_buttons) {
            if (m.code == b.code) return std::string(m.name);
        }
        return "MOUSE_" + std::to_string(b.code);
    }
    if (b.code >= GLFW_KEY_A && b.code <= GLFW_KEY_Z) return std::string(1, static_cast<char>('A' + (b.code - GLFW_KEY_A)));
    if (b.code >= GLFW_KEY_0 && b.code <= GLFW_KEY_9) return std::string(1, static_cast<char>('0' + (b.code - GLFW_KEY_0)));
    if (b.code >= GLFW_KEY_F1 && b.code <= GLFW_KEY_F12) return "F" + std::to_string(b.code - GLFW_KEY_F1 + 1);
    for (const Named& k : special_keys) {
        if (k.code == b.code) return std::string(k.name);
    }
    return "KEY_" + std::to_string(b.code);
}

std::optional<Binding> parse_binding(std::string_view text) {
    const std::string name = upper(trim(text));
    if (name.empty()) return std::nullopt;
    if (name.size() == 1) {
        if (name[0] >= 'A' && name[0] <= 'Z') return Binding::key(GLFW_KEY_A + (name[0] - 'A'));
        if (name[0] >= '0' && name[0] <= '9') return Binding::key(GLFW_KEY_0 + (name[0] - '0'));
    }
    if (name[0] == 'F' && name.size() <= 3) {
        int n = 0;
        const auto [end, ec] = std::from_chars(name.data() + 1, name.data() + name.size(), n);
        if (ec == std::errc{} && end == name.data() + name.size() && n >= 1 && n <= 12) return Binding::key(GLFW_KEY_F1 + n - 1);
    }
    for (const Named& m : mouse_buttons) {
        if (m.name == name) return Binding::mouse(m.code);
    }
    for (const Named& k : special_keys) {
        if (k.name == name) return Binding::key(k.code);
    }
    return std::nullopt;
}

ActionId ActionMap::declare(std::string name, std::string description) {
    if (const auto existing = find(name)) {
        if (m_actions[existing->value].description.empty()) m_actions[existing->value].description = std::move(description);
        return *existing;
    }
    m_actions.push_back({std::move(name), std::move(description), {}});
    return ActionId{static_cast<std::uint16_t>(m_actions.size() - 1)};
}

std::optional<ActionId> ActionMap::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < m_actions.size(); ++i) {
        if (m_actions[i].name == name) return ActionId{static_cast<std::uint16_t>(i)};
    }
    return std::nullopt;
}

ActionMap& ActionMap::bind(ActionId id, Binding binding) {
    auto& list = m_actions.at(id.value).bindings;
    if (std::ranges::find(list, binding) == list.end()) list.push_back(binding);
    return *this;
}

bool ActionMap::unbind(ActionId id, Binding binding) {
    auto& list = m_actions.at(id.value).bindings;
    const auto it = std::ranges::find(list, binding);
    if (it == list.end()) return false;
    list.erase(it);
    return true;
}

void ActionMap::unbind_all(ActionId id) { m_actions.at(id.value).bindings.clear(); }

std::vector<ActionId> ActionMap::conflicts(ActionId id) const {
    std::vector<ActionId> out;
    for (std::size_t other = 0; other < m_actions.size(); ++other) {
        if (other == id.value) continue;
        const auto& mine = m_actions[id.value].bindings;
        const bool shared = std::ranges::any_of(m_actions[other].bindings, [&](Binding b) { return std::ranges::find(mine, b) != mine.end(); });
        if (shared) out.push_back(ActionId{static_cast<std::uint16_t>(other)});
    }
    return out;
}

bool ActionMap::test(const WindowSystem::InputState& in, Binding b, int kind) noexcept {
    if (b.device == Binding::Device::Key) return kind == 0 ? in.down(b.code) : kind == 1 ? in.pressed(b.code) : in.released(b.code);
    return kind == 0 ? in.mouse_down(b.code) : kind == 1 ? in.mouse_pressed(b.code) : in.mouse_released(b.code);
}

bool ActionMap::down(const WindowSystem::InputState& in, ActionId id) const noexcept {
    return id.valid() && id.value < m_actions.size() && std::ranges::any_of(m_actions[id.value].bindings, [&](Binding b) { return test(in, b, 0); });
}
bool ActionMap::pressed(const WindowSystem::InputState& in, ActionId id) const noexcept {
    return id.valid() && id.value < m_actions.size() && std::ranges::any_of(m_actions[id.value].bindings, [&](Binding b) { return test(in, b, 1); });
}
bool ActionMap::released(const WindowSystem::InputState& in, ActionId id) const noexcept {
    return id.valid() && id.value < m_actions.size() && std::ranges::any_of(m_actions[id.value].bindings, [&](Binding b) { return test(in, b, 2); });
}

float ActionMap::axis(const WindowSystem::InputState& in, ActionId positive, ActionId negative) const noexcept {
    return (down(in, positive) ? 1.0f : 0.0f) - (down(in, negative) ? 1.0f : 0.0f);
}

std::expected<void, std::string> ActionMap::apply(std::string_view text) {
    struct Change {
        ActionId id;
        std::vector<Binding> bindings;
    };
    std::vector<Change> changes;
    int line_number = 0;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        ++line_number;
        if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        const std::size_t eq = line.find('=');
        const auto fail = [&](const std::string& what) { return std::unexpected("строка " + std::to_string(line_number) + ": " + what); };
        if (eq == std::string_view::npos) return fail("ожидалось «действие = КЛАВИША, …»");
        const std::string_view action_name = trim(line.substr(0, eq));
        const auto id = find(action_name);
        if (!id) return fail("неизвестное действие: " + std::string(action_name));
        Change change{*id, {}};
        std::string_view rest = trim(line.substr(eq + 1));
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string_view item = trim(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            if (item.empty()) continue;
            const auto binding = parse_binding(item);
            if (!binding) return fail("неизвестная клавиша: " + std::string(item));
            change.bindings.push_back(*binding);
        }
        changes.push_back(std::move(change));
    }
    for (Change& c : changes) m_actions[c.id.value].bindings = std::move(c.bindings); // всё или ничего
    return {};
}

std::string ActionMap::serialize() const {
    std::string out = "# Привязки действий: имя = КЛАВИША, КЛАВИША   (MOUSE_LEFT, MOUSE_RIGHT, SPACE, F5, A…Z, 0…9)\n";
    for (const Action& a : m_actions) {
        if (!a.description.empty()) out += "# " + a.description + '\n';
        out += a.name + " =";
        for (std::size_t i = 0; i < a.bindings.size(); ++i) out += (i == 0 ? " " : ", ") + binding_name(a.bindings[i]);
        out += '\n';
    }
    return out;
}

} // namespace Core
