#include <AudioSystem/SoundEvents.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <sstream>

namespace AudioSystem {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

const char* model_name(AttenuationModel m) {
    switch (m) {
    case AttenuationModel::None: return "none";
    case AttenuationModel::Inverse: return "inverse";
    case AttenuationModel::Linear: return "linear";
    case AttenuationModel::Exponential: return "exponential";
    }
    return "inverse";
}

bool parse_float(std::string_view s, float& out) {
    s = trim(s);
    if (s.empty()) return false;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size() && std::isfinite(out);
}

bool parse_uint(std::string_view s, std::uint32_t& out) {
    s = trim(s);
    if (s.empty()) return false;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

bool parse_bool(std::string_view s, bool& out) {
    s = trim(s);
    if (s == "true" || s == "1" || s == "yes") { out = true; return true; }
    if (s == "false" || s == "0" || s == "no") { out = false; return true; }
    return false;
}

std::string fmt_float(float v) {
    std::ostringstream os;
    os.precision(9);
    os << v;
    return os.str();
}

/// Применить `key = value` к описанию; пустая строка — успех, иначе текст ошибки.
std::string apply_key(SoundEventDef& def, std::string_view key, std::string_view value) {
    const auto bad = [&](const char* what) { return std::string("bad ") + what + " value '" + std::string(trim(value)) + "' for '" + std::string(key) + "'"; };
    float f = 0.0f;
    std::uint32_t u = 0;
    bool b = false;
    const auto number = [&](float& target) { if (!parse_float(value, f)) return false; target = f; return true; };
    const auto boolean = [&](bool& target) { if (!parse_bool(value, b)) return false; target = b; return true; };

    if (key == "clips") {
        def.clips.clear();
        std::istringstream in{std::string(value)};
        for (std::string word; in >> word;) def.clips.push_back(word);
        return def.clips.empty() ? bad("clips") : "";
    }
    if (key == "bus") { def.bus = std::string(trim(value)); return def.bus.empty() ? bad("bus") : ""; }
    if (key == "volume") return number(def.volume) && def.volume >= 0.0f ? "" : bad("number");
    if (key == "volume_var") return number(def.volume_var) && def.volume_var >= 0.0f ? "" : bad("number");
    if (key == "pitch") return number(def.pitch) && def.pitch > 0.0f ? "" : bad("number");
    if (key == "pitch_var") return number(def.pitch_var) && def.pitch_var >= 0.0f ? "" : bad("number");
    if (key == "loop") return boolean(def.loop) ? "" : bad("bool");
    if (key == "spatial") return boolean(def.spatial) ? "" : bad("bool");
    if (key == "min_distance") return number(def.min_distance) && def.min_distance > 0.0f ? "" : bad("number");
    if (key == "max_distance") return number(def.max_distance) && def.max_distance > 0.0f ? "" : bad("number");
    if (key == "rolloff") return number(def.rolloff) && def.rolloff >= 0.0f ? "" : bad("number");
    if (key == "doppler") return number(def.doppler) && def.doppler >= 0.0f ? "" : bad("number");
    if (key == "air_absorption") return number(def.air_absorption) && def.air_absorption >= 0.0f ? "" : bad("number");
    if (key == "cooldown") return number(def.cooldown) && def.cooldown >= 0.0f ? "" : bad("number");
    if (key == "reverb_send") return number(def.reverb_send) && def.reverb_send >= 0.0f && def.reverb_send <= 1.0f ? "" : bad("number");
    if (key == "fade_in") return number(def.fade_in) && def.fade_in >= 0.0f ? "" : bad("number");
    if (key == "avoid_repeat") return boolean(def.avoid_repeat) ? "" : bad("bool");
    if (key == "priority") {
        if (!parse_uint(value, u) || u > 255) return bad("priority");
        def.priority = static_cast<std::uint8_t>(u);
        return "";
    }
    if (key == "max_instances") {
        if (!parse_uint(value, u)) return bad("max_instances");
        def.max_instances = u;
        return "";
    }
    if (key == "model") {
        const std::string_view v = trim(value);
        if (v == "none") def.model = AttenuationModel::None;
        else if (v == "inverse") def.model = AttenuationModel::Inverse;
        else if (v == "linear") def.model = AttenuationModel::Linear;
        else if (v == "exponential") def.model = AttenuationModel::Exponential;
        else return bad("model");
        return "";
    }
    return "unknown key '" + std::string(key) + "'";
}

} // namespace

bool SoundEvents::add(SoundEventDef def) {
    if (def.name.empty() || def.clips.empty()) return false;
    std::string name = def.name;
    State& state = m_defs[name];
    state = State{};
    state.def = std::move(def);
    return true;
}

const SoundEventDef* SoundEvents::find(std::string_view name) const {
    const auto it = m_defs.find(std::string(name));
    return it == m_defs.end() ? nullptr : &it->second.def;
}

std::expected<std::size_t, std::string> SoundEvents::load_text(std::string_view text) {
    std::vector<SoundEventDef> parsed;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;

        if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        const auto fail = [&](const std::string& why) { return std::unexpected("line " + std::to_string(line_no) + ": " + why); };

        if (line.front() == '[') {
            if (line.back() != ']') return fail("unterminated section header");
            const std::string_view name = trim(line.substr(1, line.size() - 2));
            if (name.empty()) return fail("empty event name");
            parsed.emplace_back();
            parsed.back().name = std::string(name);
            continue;
        }
        if (parsed.empty()) return fail("key before any [event] header");
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) return fail("expected 'key = value'");
        const std::string err = apply_key(parsed.back(), trim(line.substr(0, eq)), line.substr(eq + 1));
        if (!err.empty()) return fail(err);
    }
    for (const SoundEventDef& d : parsed) {
        if (d.clips.empty()) return std::unexpected("event '" + d.name + "': no clips");
    }
    for (SoundEventDef& d : parsed) add(std::move(d));
    return parsed.size();
}

std::string SoundEvents::to_text() const {
    std::map<std::string, const SoundEventDef*> sorted;
    for (const auto& [name, state] : m_defs) sorted.emplace(name, &state.def);
    std::string out;
    for (const auto& [name, d] : sorted) {
        out += "[" + name + "]\nclips =";
        for (const std::string& c : d->clips) out += " " + c;
        out += "\nbus = " + d->bus;
        out += "\nvolume = " + fmt_float(d->volume) + "\nvolume_var = " + fmt_float(d->volume_var);
        out += "\npitch = " + fmt_float(d->pitch) + "\npitch_var = " + fmt_float(d->pitch_var);
        out += std::string("\nloop = ") + (d->loop ? "true" : "false");
        out += std::string("\nspatial = ") + (d->spatial ? "true" : "false");
        out += "\nmin_distance = " + fmt_float(d->min_distance) + "\nmax_distance = " + fmt_float(d->max_distance);
        out += "\nrolloff = " + fmt_float(d->rolloff) + "\nmodel = " + model_name(d->model);
        out += "\ndoppler = " + fmt_float(d->doppler) + "\nair_absorption = " + fmt_float(d->air_absorption);
        out += "\npriority = " + std::to_string(d->priority) + "\nmax_instances = " + std::to_string(d->max_instances);
        out += "\ncooldown = " + fmt_float(d->cooldown) + "\nreverb_send = " + fmt_float(d->reverb_send);
        out += "\nfade_in = " + fmt_float(d->fade_in);
        out += std::string("\navoid_repeat = ") + (d->avoid_repeat ? "true" : "false") + "\n\n";
    }
    return out;
}

float SoundEvents::random01() noexcept {
    m_rng ^= m_rng >> 12; m_rng ^= m_rng << 25; m_rng ^= m_rng >> 27; // xorshift64*
    return static_cast<float>((m_rng * 0x2545F4914F6CDD1Dull) >> 40) / 16777216.0f;
}

VoiceId SoundEvents::start(State& state, const Vec3* position, Vec3 velocity) {
    const SoundEventDef& def = state.def;
    const double now = m_engine->time_seconds();
    if (def.cooldown > 0.0f && now - state.last_post < static_cast<double>(def.cooldown)) return {};

    std::erase_if(state.instances, [&](VoiceId id) { return !m_engine->is_playing(id); });
    if (def.max_instances > 0 && state.instances.size() >= def.max_instances) return {};

    // Выбор клипа: среди найденных в движке, не повторяя прошлый, если есть выбор.
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < def.clips.size(); ++i) {
        if (m_engine->find_clip(def.clips[i])) candidates.push_back(i);
    }
    if (candidates.empty()) return {};
    if (def.avoid_repeat && candidates.size() > 1) std::erase(candidates, state.last_clip);
    const std::size_t chosen = candidates[std::min(candidates.size() - 1, static_cast<std::size_t>(random01() * static_cast<float>(candidates.size())))];

    PlayParams p;
    p.bus = m_engine->find_bus(def.bus);
    if (p.bus.index >= m_engine->bus_count()) p.bus = Buses::sfx;
    p.volume = std::max(0.0f, def.volume + (random01() * 2.0f - 1.0f) * def.volume_var);
    p.pitch = def.pitch * std::exp2((random01() * 2.0f - 1.0f) * def.pitch_var / 12.0f);
    p.loop = def.loop;
    p.priority = def.priority;
    p.reverb_send = def.reverb_send;
    p.fade_in = def.fade_in;
    if (def.spatial && position) {
        p.spatial.enabled = true;
        p.spatial.position = *position;
        p.spatial.velocity = velocity;
        p.spatial.min_distance = def.min_distance;
        p.spatial.max_distance = def.max_distance;
        p.spatial.rolloff = def.rolloff;
        p.spatial.model = def.model;
        p.spatial.doppler = def.doppler;
        p.spatial.air_absorption = def.air_absorption;
    }

    const VoiceId id = m_engine->play(m_engine->find_clip(def.clips[chosen]), p);
    if (!id) return {};
    state.last_post = now;
    state.last_clip = chosen;
    state.instances.push_back(id);
    return id;
}

VoiceId SoundEvents::post(std::string_view name) {
    const auto it = m_defs.find(std::string(name));
    return it == m_defs.end() ? VoiceId{} : start(it->second, nullptr, {});
}

VoiceId SoundEvents::post(std::string_view name, Vec3 position, Vec3 velocity) {
    const auto it = m_defs.find(std::string(name));
    return it == m_defs.end() ? VoiceId{} : start(it->second, &position, velocity);
}

void SoundEvents::stop(std::string_view name, float fade_seconds) {
    const auto it = m_defs.find(std::string(name));
    if (it == m_defs.end()) return;
    for (VoiceId id : it->second.instances) m_engine->stop(id, fade_seconds);
    it->second.instances.clear();
}

} // namespace AudioSystem
