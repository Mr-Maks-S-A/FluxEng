#include <Replay/Replay.hpp>

#include <Math/Assert.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace Replay {

namespace {
constexpr std::uint32_t magic = 0x52584C46; // "FLXR"
constexpr std::uint32_t version = 2; // 2: добавлена таблица схем команд; версия 1 читается

template<typename T>
void put(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}
template<typename T>
bool get(std::ifstream& in, T& value) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}
void put_string(std::ofstream& out, const std::string& s) {
    const auto len = static_cast<std::uint8_t>(std::min<std::size_t>(s.size(), 255));
    put(out, len);
    out.write(s.data(), len);
}
bool get_string(std::ifstream& in, std::string& s) {
    std::uint8_t len = 0;
    if (!get(in, len)) return false;
    s.resize(len);
    return len == 0 || static_cast<bool>(in.read(s.data(), len));
}
} // namespace

// ------------------------------------------------------------ CommandRegistry

CommandRegistry& CommandRegistry::add(CommandSchema schema) {
    FLUX_ASSERT(find(schema.type) == nullptr, "CommandRegistry::add: тип команды уже зарегистрирован");
    m_schemas.push_back(std::move(schema));
    return *this;
}

const CommandSchema* CommandRegistry::find(std::uint16_t type) const noexcept {
    for (const CommandSchema& s : m_schemas) {
        if (s.type == type) return &s;
    }
    return nullptr;
}

namespace {
std::string format_with(const std::vector<CommandSchema>& schemas, const Command& c) {
    const CommandSchema* schema = nullptr;
    for (const CommandSchema& s : schemas) {
        if (s.type == c.type) schema = &s;
    }
    char buffer[64];
    const auto value = [&](const CommandField& f, std::int32_t v) -> std::string {
        if (f.kind == CommandField::Kind::Fixed) {
            std::snprintf(buffer, sizeof buffer, "%g", static_cast<double>(v) / 65536.0);
        } else {
            std::snprintf(buffer, sizeof buffer, "%d", v);
        }
        return buffer;
    };
    if (!schema) {
        std::snprintf(buffer, sizeof buffer, "type#%u arg=%d x=%d y=%d z=%d", c.type, c.arg, c.x, c.y, c.z);
        return buffer;
    }
    std::string out = schema->name;
    const std::int32_t values[4] = {c.arg, c.x, c.y, c.z};
    for (std::size_t i = 0; i < 4; ++i) {
        if (schema->fields[i].name.empty()) continue;
        out += ' ' + schema->fields[i].name + '=' + value(schema->fields[i], values[i]);
    }
    return out;
}
} // namespace

std::string CommandRegistry::format(const Command& command) const { return format_with(m_schemas, command); }

// ------------------------------------------------------------------- Recording

void Recording::add(std::uint32_t tick, const Command& command) {
    FLUX_ASSERT(m_ticks.empty() || tick >= m_ticks.back(), "Recording::add: тики должны не убывать");
    m_ticks.push_back(tick);
    m_commands.push_back(command);
}

std::span<const Command> Recording::at(std::uint32_t tick) const noexcept {
    const auto [lo, hi] = std::ranges::equal_range(m_ticks, tick);
    return std::span<const Command>(m_commands).subspan(static_cast<std::size_t>(lo - m_ticks.begin()), static_cast<std::size_t>(hi - lo));
}

std::expected<void, std::string> Recording::save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) return std::unexpected("не удалось открыть для записи: " + path);
    put(out, magic);
    put(out, version);
    put(out, seed);
    put(out, tick_count);
    put(out, static_cast<std::uint32_t>(m_commands.size()));
    for (const std::uint64_t h : final_hashes.value) put(out, h);
    put(out, static_cast<std::uint32_t>(schemas.size()));
    for (const CommandSchema& schema : schemas) {
        put(out, schema.type);
        put_string(out, schema.name);
        for (const CommandField& f : schema.fields) {
            put_string(out, f.name);
            put(out, static_cast<std::uint8_t>(f.kind));
        }
    }
    for (std::size_t i = 0; i < m_commands.size(); ++i) {
        put(out, m_ticks[i]);
        put(out, m_commands[i]);
    }
    return out ? std::expected<void, std::string>{} : std::unexpected("ошибка записи: " + path);
}

std::expected<Recording, std::string> Recording::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected("не удалось открыть: " + path);
    std::uint32_t m = 0, v = 0, count = 0;
    Recording r;
    if (!get(in, m) || !get(in, v) || m != magic) return std::unexpected("не файл записи FluxEng: " + path);
    if (v != 1 && v != version) return std::unexpected("неизвестная версия записи: " + std::to_string(v));
    if (!get(in, r.seed) || !get(in, r.tick_count) || !get(in, count)) return std::unexpected("запись повреждена: " + path);
    for (std::uint64_t& h : r.final_hashes.value) {
        if (!get(in, h)) return std::unexpected("запись повреждена: " + path);
    }
    if (v >= 2) {
        std::uint32_t schema_count = 0;
        if (!get(in, schema_count) || schema_count > 4096) return std::unexpected("запись повреждена (таблица схем): " + path);
        for (std::uint32_t i = 0; i < schema_count; ++i) {
            CommandSchema schema;
            if (!get(in, schema.type) || !get_string(in, schema.name)) return std::unexpected("запись повреждена (таблица схем): " + path);
            for (CommandField& f : schema.fields) {
                std::uint8_t kind = 0;
                if (!get_string(in, f.name) || !get(in, kind)) return std::unexpected("запись повреждена (таблица схем): " + path);
                f.kind = static_cast<CommandField::Kind>(kind);
            }
            r.schemas.push_back(std::move(schema));
        }
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t tick = 0;
        Command c;
        if (!get(in, tick) || !get(in, c)) return std::unexpected("запись обрезана: " + path);
        if (!r.m_ticks.empty() && tick < r.m_ticks.back()) return std::unexpected("запись повреждена (тики идут назад): " + path);
        r.add(tick, c);
    }
    return r;
}

// --------------------------------------------------------------------- Session

Session::Session(Mode mode, std::uint64_t seed) : m_mode(mode) { m_recording.seed = seed; }

Session Session::record(std::uint64_t seed, std::string path, const CommandRegistry* registry) {
    Session s(Mode::Record, seed);
    s.m_path = std::move(path);
    if (registry) s.m_recording.schemas = registry->schemas();
    return s;
}

Session Session::replay(Recording recording) {
    Session s(Mode::Replay, recording.seed);
    s.m_recording = std::move(recording);
    return s;
}

std::expected<Session, std::string> Session::from_args(std::span<const std::string> args, std::uint64_t default_seed, const CommandRegistry* registry) {
    std::uint64_t seed = default_seed;
    std::string record_path, replay_path;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--record") record_path = args[i + 1];
        if (args[i] == "--replay") replay_path = args[i + 1];
        if (args[i] == "--seed") seed = std::strtoull(args[i + 1].c_str(), nullptr, 10);
    }
    if (!replay_path.empty()) {
        auto loaded = Recording::load(replay_path);
        if (!loaded) return std::unexpected(loaded.error());
        return replay(std::move(*loaded));
    }
    if (!record_path.empty()) return record(seed, std::move(record_path), registry);
    return off(seed);
}

std::span<const Command> Session::begin_tick(std::uint32_t tick, std::span<const Command> live) {
    switch (m_mode) {
    case Mode::Replay: return m_recording.at(tick);
    case Mode::Record:
        for (const Command& c : live) m_recording.add(tick, c);
        return live;
    case Mode::Off: break;
    }
    return live;
}

std::expected<Verdict, std::string> Session::finish(std::uint32_t ticks_run, const StateHashes& hashes) {
    Verdict v{.actual = hashes};
    if (m_mode == Mode::Record) {
        m_recording.tick_count = ticks_run;
        m_recording.final_hashes = hashes;
        if (!m_path.empty()) {
            if (auto saved = m_recording.save(m_path); !saved) return std::unexpected(saved.error());
        }
    } else if (m_mode == Mode::Replay) {
        v.checked = true;
        v.expected = m_recording.final_hashes;
        v.match = (ticks_run == m_recording.tick_count) && v.expected == hashes;
    }
    return v;
}

std::string inspect(const Recording& r, std::size_t max_commands) {
    std::string out;
    char buffer[200];
    std::snprintf(buffer, sizeof buffer, "seed %llu | %u ticks | %zu commands | final hashes %016llx %016llx %016llx\n", static_cast<unsigned long long>(r.seed), r.tick_count,
                  r.command_count(), static_cast<unsigned long long>(r.final_hashes.value[0]), static_cast<unsigned long long>(r.final_hashes.value[1]),
                  static_cast<unsigned long long>(r.final_hashes.value[2]));
    out += buffer;
    out += "command types:";
    if (r.schemas.empty()) out += " (нет таблицы схем: запись старого формата)";
    out += '\n';
    for (const CommandSchema& s : r.schemas) {
        out += "  #" + std::to_string(s.type) + " " + s.name;
        for (const CommandField& f : s.fields) {
            if (!f.name.empty()) out += ' ' + f.name + (f.kind == CommandField::Kind::Fixed ? ":fixed" : ":int");
        }
        out += '\n';
    }
    const auto commands = r.commands();
    const auto ticks = r.command_ticks();
    for (std::size_t i = 0; i < commands.size() && i < max_commands; ++i) {
        out += "tick " + std::to_string(ticks[i]) + ": " + format_with(r.schemas, commands[i]) + '\n';
    }
    if (commands.size() > max_commands) out += "… ещё " + std::to_string(commands.size() - max_commands) + " команд\n";
    return out;
}

std::optional<Difference> diff(const Recording& a, const Recording& b) {
    if (a.seed != b.seed) return Difference{Difference::Kind::Seed, 0, "разные сиды: " + std::to_string(a.seed) + " и " + std::to_string(b.seed)};
    const auto ca = a.commands(), cb = b.commands();
    const auto ta = a.command_ticks(), tb = b.command_ticks();
    const std::size_t common = std::min(ca.size(), cb.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (ta[i] != tb[i] || !(ca[i] == cb[i])) {
            return Difference{Difference::Kind::Command, std::min(ta[i], tb[i]),
                              "команда №" + std::to_string(i) + " расходится: a — тик " + std::to_string(ta[i]) + " " + format_with(a.schemas, ca[i]) + ", b — тик " +
                                  std::to_string(tb[i]) + " " + format_with(b.schemas, cb[i])};
        }
    }
    if (ca.size() != cb.size()) {
        return Difference{Difference::Kind::CommandCount, 0, "разное число команд: " + std::to_string(ca.size()) + " и " + std::to_string(cb.size())};
    }
    if (a.tick_count != b.tick_count) {
        return Difference{Difference::Kind::TickCount, 0, "разная длина: " + std::to_string(a.tick_count) + " и " + std::to_string(b.tick_count) + " тиков"};
    }
    if (!(a.final_hashes == b.final_hashes)) {
        return Difference{Difference::Kind::FinalHash, a.tick_count, "команды совпадают, но хеши на последнем тике разные: симуляция недетерминирована или версии кода разные"};
    }
    return std::nullopt;
}

std::string describe(const Verdict& v, Session::Mode mode, std::uint32_t ticks, std::size_t commands) {
    char buffer[256];
    const auto hex = [](std::uint64_t h) { return static_cast<unsigned long long>(h); };
    if (v.checked) {
        std::snprintf(buffer, sizeof buffer, "replay: hashes %s the recording (terrain %016llx, mana %016llx, ecs %016llx)", v.match ? "MATCH" : "DIFFER FROM",
                      hex(v.expected.value[0]), hex(v.expected.value[1]), hex(v.expected.value[2]));
        return buffer;
    }
    if (mode == Session::Mode::Record) {
        std::snprintf(buffer, sizeof buffer, "recorded %u ticks, %zu commands", ticks, commands);
        return buffer;
    }
    return {};
}

// -------------------------------------------------------------- FlightRecorder

void FlightRecorder::push(std::uint32_t tick, std::span<const Command> commands, const StateHashes& hashes) noexcept {
    TickRecord& r = m_ring[m_next];
    r.tick = tick;
    r.command_count = static_cast<std::uint32_t>(commands.size());
    r.commands = {};
    std::copy_n(commands.begin(), std::min(commands.size(), max_commands), r.commands.begin());
    r.hashes = hashes;
    m_next = (m_next + 1) % m_ring.size();
    m_size = std::min(m_size + 1, m_ring.size());
}

std::vector<FlightRecorder::TickRecord> FlightRecorder::snapshot() const {
    std::vector<TickRecord> out;
    out.reserve(m_size);
    const std::size_t start = (m_next + m_ring.size() - m_size) % m_ring.size();
    for (std::size_t i = 0; i < m_size; ++i) out.push_back(m_ring[(start + i) % m_ring.size()]);
    return out;
}

bool FlightRecorder::dump(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# FlightRecorder: последние %zu тиков (от старого к новому)\n", m_size);
    for (const TickRecord& r : snapshot()) {
        std::fprintf(f, "tick %u  hashes %016llx %016llx %016llx  commands %u\n", r.tick, static_cast<unsigned long long>(r.hashes.value[0]),
                     static_cast<unsigned long long>(r.hashes.value[1]), static_cast<unsigned long long>(r.hashes.value[2]), r.command_count);
        for (std::uint32_t i = 0; i < std::min<std::uint32_t>(r.command_count, max_commands); ++i) {
            const Command& c = r.commands[i];
            std::fprintf(f, "    cmd type=%u arg=%d x=%d y=%d z=%d\n", c.type, c.arg, c.x, c.y, c.z);
        }
    }
    std::fclose(f);
    return true;
}

namespace {
const FlightRecorder* g_installed = nullptr;
std::string g_dump_path;
Math::AssertHandler g_previous = nullptr;

void on_assert(const char* expression, const char* message, const char* file, int line) {
    if (g_installed && g_installed->dump(g_dump_path)) {
        std::fprintf(stderr, "FlightRecorder: последние тики записаны в %s\n", g_dump_path.c_str());
    }
    if (g_previous) g_previous(expression, message, file, line);
}
} // namespace

void FlightRecorder::install_assert_dump(std::string path) {
    g_installed = this;
    g_dump_path = std::move(path);
    g_previous = Math::set_assert_handler(&on_assert);
}

void FlightRecorder::uninstall_assert_dump() noexcept {
    if (g_installed != this) return;
    Math::set_assert_handler(g_previous);
    g_installed = nullptr;
    g_previous = nullptr;
}

} // namespace Replay
