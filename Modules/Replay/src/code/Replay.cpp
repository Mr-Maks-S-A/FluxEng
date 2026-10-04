#include <Replay/Replay.hpp>

#include <Math/Assert.hpp>
#include <Math/Hash.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace Replay {

namespace {

// ---- Формат записи: журнал EventLog; типы записей журнала ----
constexpr std::uint32_t type_meta = 1;    // версия(1) + сид(8)
constexpr std::uint32_t type_schema = 2;  // схема команды
constexpr std::uint32_t type_command = 3; // команда (тик — в записи журнала)
constexpr std::uint32_t type_trailer = 4; // число тиков + хеши подсистем: запись закончена штатно
constexpr std::uint32_t type_blob = 5;    // хеш(8) + байты: данные, на которые ссылаются команды
constexpr std::uint8_t recording_version = 1;
constexpr std::uint32_t flush_interval_ticks = 60; // раз в секунду симуляции

/// Журнал записей небольшой: блоки по 256 байт, 4 + 2 (теряется любой 1/3 блоков полосы без потери данных).
constexpr EventLog::Config journal_config{.data_blocks = 4, .parity_blocks = 2, .block_size = 256};

constexpr char legacy_magic[4] = {'F', 'L', 'X', 'R'}; // старый плоский файл (версии 1 и 2): 0x52584C46 в little-endian

// ---- Сборка и разбор полезной нагрузки записей ----
struct Bytes {
    std::vector<std::byte> data;
    void u8(std::uint8_t v) { data.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) { for (int i = 0; i < 2; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
    void str(const std::string& s) {
        const auto len = static_cast<std::uint8_t>(std::min<std::size_t>(s.size(), 255));
        u8(len);
        for (std::uint8_t i = 0; i < len; ++i) u8(static_cast<std::uint8_t>(s[i]));
    }
};

struct Cursor {
    std::span<const std::byte> data;
    std::size_t at = 0;
    bool ok = true;
    std::uint64_t raw(int bytes) {
        if (at + static_cast<std::size_t>(bytes) > data.size()) { ok = false; return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[at++])) << (8 * i);
        return v;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(raw(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(raw(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(raw(4)); }
    std::uint64_t u64() { return raw(8); }
    std::string str() {
        const std::uint8_t len = u8();
        std::string s;
        for (std::uint8_t i = 0; i < len && ok; ++i) s.push_back(static_cast<char>(u8()));
        return s;
    }
};

Bytes encode_command(const Command& c) {
    Bytes b;
    b.u16(c.type);
    b.u16(static_cast<std::uint16_t>(c.arg));
    b.u32(static_cast<std::uint32_t>(c.x));
    b.u32(static_cast<std::uint32_t>(c.y));
    b.u32(static_cast<std::uint32_t>(c.z));
    return b;
}

std::optional<Command> decode_command(std::span<const std::byte> payload) {
    Cursor c{payload};
    Command out;
    out.type = c.u16();
    out.arg = static_cast<std::int16_t>(c.u16());
    out.x = static_cast<std::int32_t>(c.u32());
    out.y = static_cast<std::int32_t>(c.u32());
    out.z = static_cast<std::int32_t>(c.u32());
    return c.ok && c.at == payload.size() ? std::optional<Command>(out) : std::nullopt;
}

Bytes encode_blob(const Blob& blob) {
    Bytes b;
    b.u64(blob.hash);
    b.data.insert(b.data.end(), blob.bytes.begin(), blob.bytes.end());
    return b;
}

/// Блоб с несовпавшим хешем отбрасывается: подмена содержимого не должна пройти в повтор.
std::optional<Blob> decode_blob(std::span<const std::byte> payload) {
    Cursor c{payload};
    Blob blob;
    blob.hash = c.u64();
    if (!c.ok) return std::nullopt;
    blob.bytes.assign(payload.begin() + 8, payload.end());
    if (content_hash(blob.bytes) != blob.hash) return std::nullopt;
    return blob;
}

Bytes encode_meta(std::uint64_t seed) {
    Bytes b;
    b.u8(recording_version);
    b.u64(seed);
    return b;
}

Bytes encode_schema(const CommandSchema& s) {
    Bytes b;
    b.u16(s.type);
    b.str(s.name);
    for (const CommandField& f : s.fields) {
        b.str(f.name);
        b.u8(static_cast<std::uint8_t>(f.kind));
    }
    return b;
}

std::optional<CommandSchema> decode_schema(std::span<const std::byte> payload) {
    Cursor c{payload};
    CommandSchema s;
    s.type = c.u16();
    s.name = c.str();
    for (CommandField& f : s.fields) {
        f.name = c.str();
        f.kind = static_cast<CommandField::Kind>(c.u8());
    }
    return c.ok ? std::optional<CommandSchema>(s) : std::nullopt;
}

Bytes encode_trailer(std::uint32_t ticks, const StateHashes& hashes) {
    Bytes b;
    b.u32(ticks);
    b.u8(static_cast<std::uint8_t>(hashes.count()));
    for (const NamedHash& h : hashes.entries()) {
        for (const char ch : h.name) b.u8(static_cast<std::uint8_t>(ch));
        b.u64(h.value);
    }
    return b;
}

bool decode_trailer(std::span<const std::byte> payload, std::uint32_t& ticks, StateHashes& hashes) {
    Cursor c{payload};
    ticks = c.u32();
    const std::uint8_t count = c.u8();
    for (std::uint8_t i = 0; i < count && c.ok; ++i) {
        char name[16];
        for (char& ch : name) ch = static_cast<char>(c.u8());
        name[15] = '\0';
        hashes.add(name, c.u64());
    }
    return c.ok;
}


std::string format_with(const std::vector<CommandSchema>& schemas, const Command& c) {
    const CommandSchema* schema = nullptr;
    for (const CommandSchema& s : schemas) {
        if (s.type == c.type) schema = &s;
    }
    char buffer[64];
    const auto value = [&](const CommandField& f, std::int32_t v) -> std::string {
        if (f.kind == CommandField::Kind::Fixed) std::snprintf(buffer, sizeof buffer, "%g", static_cast<double>(v) / 65536.0);
        else std::snprintf(buffer, sizeof buffer, "%d", v);
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

/// Читает старый плоский файл (версии 1 и 2: без журнала). Хеши получают имена h0, h1, h2.
std::expected<Recording, std::string> load_legacy(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    const auto get = [&in](auto& value) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value))); };
    const auto get_string = [&](std::string& s) {
        std::uint8_t len = 0;
        if (!get(len)) return false;
        s.resize(len);
        return len == 0 || static_cast<bool>(in.read(s.data(), len));
    };
    std::uint32_t magic = 0, version = 0, count = 0;
    Recording r;
    if (!get(magic) || !get(version)) return std::unexpected("не файл записи FluxEng: " + path);
    if (version != 1 && version != 2) return std::unexpected("неизвестная версия записи: " + std::to_string(version));
    if (!get(r.seed) || !get(r.tick_count) || !get(count)) return std::unexpected("запись повреждена: " + path);
    static constexpr const char* legacy_names[3] = {"h0", "h1", "h2"};
    for (const char* name : legacy_names) {
        std::uint64_t h = 0;
        if (!get(h)) return std::unexpected("запись повреждена: " + path);
        r.final_hashes.add(name, h);
    }
    if (version >= 2) {
        std::uint32_t schema_count = 0;
        if (!get(schema_count) || schema_count > 4096) return std::unexpected("запись повреждена (таблица схем): " + path);
        for (std::uint32_t i = 0; i < schema_count; ++i) {
            CommandSchema schema;
            if (!get(schema.type) || !get_string(schema.name)) return std::unexpected("запись повреждена (таблица схем): " + path);
            for (CommandField& f : schema.fields) {
                std::uint8_t kind = 0;
                if (!get_string(f.name) || !get(kind)) return std::unexpected("запись повреждена (таблица схем): " + path);
                f.kind = static_cast<CommandField::Kind>(kind);
            }
            r.schemas.push_back(std::move(schema));
        }
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t tick = 0;
        Command c;
        if (!get(tick) || !get(c)) return std::unexpected("запись обрезана: " + path);
        r.add(tick, c);
    }
    return r;
}

/// Пишет запись в журнал целиком (для Recording::save).
void write_recording(EventLog::Writer& w, const Recording& r) {
    const Bytes meta = encode_meta(r.seed);
    w.append(0, type_meta, meta.data);
    for (const CommandSchema& s : r.schemas) w.append(0, type_schema, encode_schema(s).data);
    for (const Blob& blob : r.blobs) w.append(0, type_blob, encode_blob(blob).data);
    for (std::size_t i = 0; i < r.command_count(); ++i) w.append(r.command_ticks()[i], type_command, encode_command(r.commands()[i]).data);
    if (r.complete) w.append(r.tick_count, type_trailer, encode_trailer(r.tick_count, r.final_hashes).data);
}

} // namespace

std::uint64_t content_hash(std::span<const std::byte> bytes) noexcept {
    return Math::content_hash(bytes);
}

// -------------------------------------------------------------------------------------------- StateHashes

StateHashes& StateHashes::add(std::string_view name, std::uint64_t value) {
    FLUX_ASSERT(m_count < capacity, "StateHashes::add: больше 8 подсистем");
    FLUX_ASSERT(!name.empty() && name.size() <= 15, "StateHashes::add: имя подсистемы — от 1 до 15 символов");
    FLUX_ASSERT(!find(name).has_value(), "StateHashes::add: имя подсистемы уже занято");
    NamedHash& h = m_entries[m_count++];
    std::memcpy(h.name.data(), name.data(), name.size());
    h.value = value;
    return *this;
}

std::optional<std::uint64_t> StateHashes::find(std::string_view name) const noexcept {
    for (const NamedHash& h : entries()) {
        if (h.name_view() == name) return h.value;
    }
    return std::nullopt;
}

std::vector<std::string> StateHashes::differing(const StateHashes& other) const {
    std::vector<std::string> out;
    for (const NamedHash& h : entries()) {
        if (const auto v = other.find(h.name_view()); !v || *v != h.value) out.emplace_back(h.name_view());
    }
    for (const NamedHash& h : other.entries()) {
        if (!find(h.name_view())) out.emplace_back(h.name_view());
    }
    return out;
}

std::string StateHashes::describe() const {
    std::string out;
    char buffer[40];
    for (const NamedHash& h : entries()) {
        std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(h.value));
        if (!out.empty()) out += ' ';
        out += std::string(h.name_view()) + '=' + buffer;
    }
    return out;
}

// -------------------------------------------------------------------------------------- CommandRegistry

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

std::string CommandRegistry::format(const Command& command) const { return format_with(m_schemas, command); }

// ----------------------------------------------------------------------------------------- Recording

void Recording::add(std::uint32_t tick, const Command& command) {
    FLUX_ASSERT(m_ticks.empty() || tick >= m_ticks.back(), "Recording::add: тики должны не убывать");
    m_ticks.push_back(tick);
    m_commands.push_back(command);
}

std::uint64_t Recording::add_blob(std::span<const std::byte> bytes) {
    const std::uint64_t hash = content_hash(bytes);
    if (!find_blob(hash)) blobs.push_back(Blob{hash, std::vector<std::byte>(bytes.begin(), bytes.end())});
    return hash;
}

const Blob* Recording::find_blob(std::uint64_t hash) const noexcept {
    for (const Blob& b : blobs) {
        if (b.hash == hash) return &b;
    }
    return nullptr;
}

std::span<const Command> Recording::at(std::uint32_t tick) const noexcept {
    const auto [lo, hi] = std::ranges::equal_range(m_ticks, tick);
    return std::span<const Command>(m_commands).subspan(static_cast<std::size_t>(lo - m_ticks.begin()), static_cast<std::size_t>(hi - lo));
}

std::expected<void, std::string> Recording::save(const std::string& path) const {
    auto storage = EventLog::FileStorage::open(path, EventLog::FileStorage::Mode::Create);
    if (!storage) return std::unexpected("не удалось открыть для записи: " + path);
    auto writer = EventLog::Writer::create(*storage, journal_config);
    if (!writer) return std::unexpected(writer.error());
    write_recording(*writer, *this);
    if (!writer->flush()) return std::unexpected("ошибка записи: " + path);
    return {};
}

std::expected<std::pair<Recording, LoadInfo>, std::string> Recording::load_with_info(const std::string& path) {
    {
        std::ifstream in(path, std::ios::binary);
        char head[4] = {};
        if (!in || !in.read(head, 4)) return std::unexpected("не удалось открыть: " + path);
        if (std::memcmp(head, legacy_magic, 4) == 0) {
            auto r = load_legacy(path);
            if (!r) return std::unexpected(r.error());
            return std::pair<Recording, LoadInfo>{std::move(*r), LoadInfo{.format = LoadInfo::Format::Legacy}};
        }
    }
    auto storage = EventLog::FileStorage::open(path, EventLog::FileStorage::Mode::Existing);
    if (!storage) return std::unexpected("не удалось открыть: " + path);
    auto read = EventLog::read_all(*storage, /*repair=*/false);
    if (!read) return std::unexpected("не файл записи FluxEng: " + path + " (" + read.error() + ")");

    Recording r;
    LoadInfo info;
    bool have_meta = false, have_trailer = false;
    std::uint32_t trailer_ticks = 0;
    for (const EventLog::Record& rec : read->records) {
        switch (rec.type) {
        case type_meta: {
            Cursor c{rec.payload};
            const std::uint8_t version = c.u8();
            r.seed = c.u64();
            if (!c.ok || version != recording_version) return std::unexpected("неизвестная версия записи в журнале: " + path);
            have_meta = true;
            break;
        }
        case type_schema:
            if (auto s = decode_schema(rec.payload)) r.schemas.push_back(std::move(*s));
            break;
        case type_blob:
            if (auto blob = decode_blob(rec.payload)) {
                if (!r.find_blob(blob->hash)) r.blobs.push_back(std::move(*blob));
            }
            break;
        case type_command:
            if (const auto c = decode_command(rec.payload)) r.add(rec.tick, *c);
            break;
        case type_trailer:
            have_trailer = decode_trailer(rec.payload, trailer_ticks, r.final_hashes);
            break;
        default: break; // записи неизвестных типов (из будущих версий) пропускаются
        }
    }
    if (!have_meta) return std::unexpected("запись не содержит заголовка (сид потерян): " + path);
    r.complete = have_trailer;
    r.tick_count = have_trailer ? trailer_ticks : (r.command_count() == 0 ? 0 : r.command_ticks().back() + 1); // оборвана: ровно до последней команды
    info.complete = have_trailer;
    info.recovery = std::move(read->report);
    return std::pair<Recording, LoadInfo>{std::move(r), std::move(info)};
}

std::expected<Recording, std::string> Recording::load(const std::string& path) {
    auto loaded = load_with_info(path);
    if (!loaded) return std::unexpected(loaded.error());
    if (loaded->second.recovery.records_lost > 0) { // команды потеряны: повтор разошёлся бы молча — не допускаем
        return std::unexpected("запись потеряла " + std::to_string(loaded->second.recovery.records_lost) + " записей (повреждение сверх избыточности): повтор невозможен: " + path);
    }
    return std::move(loaded->first);
}

std::expected<EventLog::RecoveryReport, std::string> Recording::repair_file(const std::string& path) {
    auto storage = EventLog::FileStorage::open(path, EventLog::FileStorage::Mode::Existing);
    if (!storage) return std::unexpected("не удалось открыть: " + path);
    return EventLog::repair(*storage);
}

// ------------------------------------------------------------------------------------------- Session

Session::Session(Mode mode, std::uint64_t seed) : m_mode(mode) { m_recording.seed = seed; }
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default; // Writer дописывает недописанную полосу: оборванная запись остаётся читаемой

std::expected<Session, std::string> Session::record(std::uint64_t seed, std::string path, const CommandRegistry* registry) {
    Session s(Mode::Record, seed);
    if (registry) s.m_recording.schemas = registry->schemas();
    if (!path.empty()) {
        s.m_storage = EventLog::FileStorage::open(path, EventLog::FileStorage::Mode::Create);
        if (!s.m_storage) return std::unexpected("не удалось создать файл записи: " + path);
        auto writer = EventLog::Writer::create(*s.m_storage, journal_config);
        if (!writer) return std::unexpected(writer.error());
        s.m_writer = std::make_unique<EventLog::Writer>(std::move(*writer));
        s.m_writer->append(0, type_meta, encode_meta(seed).data);
        for (const CommandSchema& schema : s.m_recording.schemas) s.m_writer->append(0, type_schema, encode_schema(schema).data);
        (void)s.m_writer->flush(); // заголовок и схемы на диске с самого начала
    }
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

std::uint64_t Session::add_blob(std::uint32_t tick, std::span<const std::byte> bytes) {
    const std::uint64_t hash = content_hash(bytes);
    if (m_mode != Mode::Record || m_recording.find_blob(hash)) return hash;
    m_recording.add_blob(bytes);
    if (m_writer) m_writer->append(tick, type_blob, encode_blob(*m_recording.find_blob(hash)).data);
    return hash;
}

std::span<const Command> Session::begin_tick(std::uint32_t tick, std::span<const Command> live) {
    switch (m_mode) {
    case Mode::Replay: return m_recording.at(tick);
    case Mode::Record:
        for (const Command& c : live) {
            m_recording.add(tick, c);
            if (m_writer) m_writer->append(tick, type_command, encode_command(c).data);
        }
        if (m_writer && tick >= m_last_flush + flush_interval_ticks) { // раз в секунду симуляции: при сбое теряется не больше неё
            (void)m_writer->flush();
            m_last_flush = tick;
        }
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
        m_recording.complete = true;
        if (m_writer) {
            m_writer->append(ticks_run, type_trailer, encode_trailer(ticks_run, hashes).data); // трейлер: запись закончена штатно
            if (!m_writer->flush()) return std::unexpected("ошибка записи файла записи");
            m_writer.reset();
            m_storage.reset();
        }
    } else if (m_mode == Mode::Replay) {
        v.expected = m_recording.final_hashes;
        v.complete = m_recording.complete;
        v.checked = !v.expected.empty();
        v.differing = v.expected.differing(hashes);
        v.match = !v.checked || (ticks_run == m_recording.tick_count && v.differing.empty());
    }
    return v;
}

// ------------------------------------------------------------------------------------------ Инструменты

std::string inspect(const Recording& r, std::size_t max_commands) {
    std::string out;
    char buffer[200];
    std::snprintf(buffer, sizeof buffer, "seed %llu | %u ticks | %zu commands%s\n", static_cast<unsigned long long>(r.seed), r.tick_count, r.command_count(),
                  r.complete ? "" : " | ЗАПИСЬ ОБОРВАНА: нет финальных хешей, повтор не сверяется");
    out += buffer;
    out += "final hashes: " + (r.final_hashes.empty() ? std::string("(нет)") : r.final_hashes.describe()) + '\n';
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

std::string describe(const LoadInfo& info) {
    std::string out = info.format == LoadInfo::Format::Legacy ? "формат: старый плоский файл (без избыточности)\n" : "формат: журнал EventLog (с избыточностью)\n";
    if (info.format == LoadInfo::Format::Journal) {
        const EventLog::RecoveryReport& r = info.recovery;
        out += "блоков: " + std::to_string(r.blocks_total) + ", испорчено " + std::to_string(r.blocks_corrupt) + ", восстановлено " + std::to_string(r.blocks_repaired) +
               ", потеряно " + std::to_string(r.blocks_lost) + "; записей потеряно " + std::to_string(r.records_lost) + '\n';
        if (r.header_repaired) out += "одна копия заголовка журнала была повреждена\n";
        for (const EventLog::Gap& g : r.gaps) out += "пропуск записей " + std::to_string(g.first_sequence) + "…" + std::to_string(g.last_sequence) + '\n';
    }
    if (!info.complete) out += "запись оборвана: финальных хешей нет\n";
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
    if (ca.size() != cb.size()) return Difference{Difference::Kind::CommandCount, 0, "разное число команд: " + std::to_string(ca.size()) + " и " + std::to_string(cb.size())};
    if (a.tick_count != b.tick_count) return Difference{Difference::Kind::TickCount, 0, "разная длина: " + std::to_string(a.tick_count) + " и " + std::to_string(b.tick_count) + " тиков"};
    if (!(a.final_hashes == b.final_hashes)) {
        std::string names;
        for (const std::string& n : a.final_hashes.differing(b.final_hashes)) names += (names.empty() ? "" : ", ") + n;
        return Difference{Difference::Kind::FinalHash, a.tick_count,
                          "команды совпадают, но хеши на последнем тике разные — разошлись подсистемы: " + names + " (симуляция недетерминирована или версии кода разные)"};
    }
    return std::nullopt;
}

std::string describe(const Verdict& v, Session::Mode mode, std::uint32_t ticks, std::size_t commands) {
    char buffer[128];
    if (mode == Session::Mode::Replay && !v.checked) return "replay: запись оборвана — финальных хешей нет, сверять нечего (прогон выполнен до последней команды)";
    if (v.checked) {
        std::string text = std::string("replay: hashes ") + (v.match ? "MATCH" : "DIFFER FROM") + " the recording (" + v.expected.describe() + ")";
        if (!v.match && !v.differing.empty()) {
            text += " — разошлись подсистемы:";
            for (const std::string& n : v.differing) text += ' ' + n;
        }
        return text;
    }
    if (mode == Session::Mode::Record) {
        std::snprintf(buffer, sizeof buffer, "recorded %u ticks, %zu commands", ticks, commands);
        return buffer;
    }
    return {};
}

// -------------------------------------------------------------------------------------- FlightRecorder

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
        std::fprintf(f, "tick %u  hashes %s  commands %u\n", r.tick, r.hashes.describe().c_str(), r.command_count);
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
    if (g_installed && g_installed->dump(g_dump_path)) std::fprintf(stderr, "FlightRecorder: последние тики записаны в %s\n", g_dump_path.c_str());
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
