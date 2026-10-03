#include <EventLog/Journal.hpp>

#include <Math/Assert.hpp>
#include <Math/Crc32c.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace EventLog {

namespace {

constexpr std::uint32_t file_magic = 0x4A4C5846; // «FXLJ»
constexpr std::uint16_t file_version = 1;
constexpr std::size_t header_bytes = 32;
constexpr std::size_t base_offset = 2 * header_bytes; // две копии заголовка
constexpr std::size_t block_overhead = 12;            // crc(4) + номер полосы(4) + номер блока(2) + резерв(2)
constexpr std::size_t record_header = 22;             // sync(2) + длина(4) + номер(8) + тик(4) + тип(4)
constexpr std::size_t record_trailer = 4;             // CRC-32C записи
constexpr std::uint32_t max_payload = 1u << 20;
constexpr std::byte sync0{0x5A}, sync1{0xA5};

template<typename T>
void put(std::byte* at, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) at[i] = static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFF);
}
template<typename T>
T get(const std::byte* at) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(at[i])) << (8 * i);
    return static_cast<T>(v);
}

std::expected<void, std::string> validate(const Config& c) {
    if (c.data_blocks < 1 || c.parity_blocks < 1 || c.data_blocks + c.parity_blocks > 255) return std::unexpected("Config: нужно 1 ≤ k, 1 ≤ m, k + m ≤ 255");
    if (c.block_size < 64 || c.block_size > (1u << 20)) return std::unexpected("Config: block_size должен быть от 64 байт до 1 МиБ");
    return {};
}

std::size_t wire_size(const Config& c) { return block_overhead + c.block_size; }
std::size_t stripe_bytes(const Config& c) { return static_cast<std::size_t>(c.data_blocks + c.parity_blocks) * wire_size(c); }

std::array<std::byte, header_bytes> make_header(const Config& c) {
    std::array<std::byte, header_bytes> h{};
    put<std::uint32_t>(h.data() + 0, file_magic);
    put<std::uint16_t>(h.data() + 4, file_version);
    put<std::uint16_t>(h.data() + 6, c.data_blocks);
    put<std::uint16_t>(h.data() + 8, c.parity_blocks);
    put<std::uint32_t>(h.data() + 12, c.block_size);
    put<std::uint32_t>(h.data() + 28, Math::crc32c(std::span<const std::byte>(h.data(), 28)));
    return h;
}

std::optional<Config> parse_header(std::span<const std::byte> h) {
    if (h.size() < header_bytes || get<std::uint32_t>(h.data()) != file_magic || get<std::uint16_t>(h.data() + 4) != file_version) return std::nullopt;
    if (get<std::uint32_t>(h.data() + 28) != Math::crc32c(h.first(28))) return std::nullopt;
    Config c{get<std::uint16_t>(h.data() + 6), get<std::uint16_t>(h.data() + 8), get<std::uint32_t>(h.data() + 12)};
    return validate(c) ? std::optional<Config>(c) : std::nullopt;
}

/// Блок на проводе: [crc][полоса][номер блока][резерв][данные]. CRC покрывает всё после себя.
void write_block(std::byte* at, const Config& c, std::uint64_t stripe, int block, std::span<const std::byte> payload) {
    put<std::uint32_t>(at + 4, static_cast<std::uint32_t>(stripe));
    put<std::uint16_t>(at + 8, static_cast<std::uint16_t>(block));
    put<std::uint16_t>(at + 10, 0);
    std::memcpy(at + block_overhead, payload.data(), c.block_size);
    put<std::uint32_t>(at, Math::crc32c(std::span<const std::byte>(at + 4, wire_size(c) - 4)));
}

bool block_valid(const std::byte* at, const Config& c, std::uint64_t stripe, int block) {
    return get<std::uint32_t>(at) == Math::crc32c(std::span<const std::byte>(at + 4, wire_size(c) - 4)) && get<std::uint32_t>(at + 4) == static_cast<std::uint32_t>(stripe) &&
           get<std::uint16_t>(at + 8) == static_cast<std::uint16_t>(block);
}

} // namespace

// ------------------------------------------------------------------------------------------------- чтение

std::expected<ReadResult, std::string> read_all(Storage& storage, bool repair_in_place) {
    ReadResult result;
    RecoveryReport& report = result.report;

    // 1. Заголовок: две копии, достаточно одной.
    std::array<std::byte, base_offset> head{};
    const std::size_t got = storage.read(0, head);
    const auto copy_a = parse_header(std::span<const std::byte>(head.data(), got >= header_bytes ? header_bytes : 0));
    const auto copy_b = parse_header(std::span<const std::byte>(head.data() + header_bytes, got >= base_offset ? header_bytes : 0));
    if (!copy_a && !copy_b) return std::unexpected("журнал: обе копии заголовка разрушены — форма журнала неизвестна");
    const Config config = copy_a ? *copy_a : *copy_b;
    if (!copy_a || !copy_b) {
        report.header_repaired = true;
        if (repair_in_place) {
            const auto fixed = make_header(config);
            (void)storage.write(0, fixed);
            (void)storage.write(header_bytes, fixed);
        }
    }

    // 2. Полосы. Недописанная в конце (обрыв) считается полосой с недостающими блоками.
    const ErasureCode code(config.data_blocks, config.parity_blocks);
    const int n = config.data_blocks + config.parity_blocks;
    const std::size_t wire = wire_size(config), stripe_size = stripe_bytes(config), bs = config.block_size;
    const std::uint64_t total = storage.size();
    report.stripes = total > base_offset ? (total - base_offset + stripe_size - 1) / stripe_size : 0;

    std::vector<std::byte> stream; // логический поток из блоков данных
    std::vector<char> hole;        // по блоку потока: 1 — блок потерян
    std::vector<std::byte> raw(stripe_size);
    std::vector<std::vector<std::byte>> payload(static_cast<std::size_t>(n), std::vector<std::byte>(bs));
    for (std::uint64_t s = 0; s < report.stripes; ++s) {
        const std::uint64_t offset = base_offset + s * stripe_size;
        std::ranges::fill(raw, std::byte{0});
        const std::size_t have = storage.read(offset, raw);
        std::array<bool, 256> ok{};
        std::vector<std::span<std::byte>> blocks;
        int bad = 0;
        for (int b = 0; b < n; ++b) {
            const std::byte* at = raw.data() + static_cast<std::size_t>(b) * wire;
            const bool complete = have >= (static_cast<std::size_t>(b) + 1) * wire;
            ok[static_cast<std::size_t>(b)] = complete && block_valid(at, config, s, b);
            if (ok[static_cast<std::size_t>(b)]) std::memcpy(payload[static_cast<std::size_t>(b)].data(), at + block_overhead, bs);
            else ++bad;
            blocks.emplace_back(payload[static_cast<std::size_t>(b)]);
        }
        report.blocks_total += static_cast<std::uint64_t>(n);
        report.blocks_corrupt += static_cast<std::uint64_t>(bad);
        bool recovered = true;
        if (bad > 0) {
            ++report.stripes_damaged;
            recovered = code.reconstruct(blocks, std::span<const bool>(ok.data(), static_cast<std::size_t>(n)));
            if (recovered) {
                report.blocks_repaired += static_cast<std::uint64_t>(bad);
                if (repair_in_place) { // переписываем только плохие блоки: заново с заголовком и CRC
                    std::vector<std::byte> wire_block(wire);
                    for (int b = 0; b < n; ++b) {
                        if (ok[static_cast<std::size_t>(b)]) continue;
                        write_block(wire_block.data(), config, s, b, payload[static_cast<std::size_t>(b)]);
                        (void)storage.write(offset + static_cast<std::uint64_t>(b) * wire, wire_block);
                    }
                }
            } else {
                ++report.stripes_lost;
                report.blocks_lost += static_cast<std::uint64_t>(bad);
            }
        }
        for (int b = 0; b < config.data_blocks; ++b) { // в поток идут блоки данных; потерянные — дыры
            const bool present = recovered || ok[static_cast<std::size_t>(b)];
            stream.insert(stream.end(), payload[static_cast<std::size_t>(b)].begin(), payload[static_cast<std::size_t>(b)].end());
            hole.push_back(present ? 0 : 1);
        }
    }
    if (repair_in_place) (void)storage.flush();

    // 3. Записи. Ищем маркер sync, проверяем границы и CRC; на дыре и на мусоре «перепрыгиваем» к следующему кандидату.
    const std::size_t size = stream.size();
    const auto hole_between = [&](std::size_t first, std::size_t last_exclusive) {
        for (std::size_t block = first / bs; block <= (last_exclusive - 1) / bs && block < hole.size(); ++block) {
            if (hole[block]) return true;
        }
        return false;
    };
    std::size_t pos = 0;
    std::uint64_t last_sequence = 0;
    bool have_last = false;
    while (pos + record_header + record_trailer <= size) {
        if (hole[pos / bs]) { // потерянный блок: к началу следующего
            pos = (pos / bs + 1) * bs;
            continue;
        }
        if (stream[pos] == sync0 && stream[pos + 1] == sync1) {
            const std::uint32_t length = get<std::uint32_t>(&stream[pos + 2]);
            const std::size_t end = pos + record_header + length + record_trailer;
            if (length <= max_payload && end <= size && !hole_between(pos, end) &&
                get<std::uint32_t>(&stream[end - record_trailer]) == Math::crc32c(std::span<const std::byte>(&stream[pos], end - record_trailer - pos))) {
                Record r;
                r.sequence = get<std::uint64_t>(&stream[pos + 6]);
                r.tick = get<std::uint32_t>(&stream[pos + 14]);
                r.type = get<std::uint32_t>(&stream[pos + 18]);
                r.payload.assign(stream.begin() + static_cast<std::ptrdiff_t>(pos + record_header), stream.begin() + static_cast<std::ptrdiff_t>(end - record_trailer));
                if (!have_last || r.sequence > last_sequence) { // дубль (ложная находка внутри данных) игнорируется
                    if (r.sequence > (have_last ? last_sequence + 1 : 0)) {
                        report.gaps.push_back({have_last ? last_sequence + 1 : 0, r.sequence - 1});
                        report.records_lost += r.sequence - (have_last ? last_sequence + 1 : 0);
                    }
                    last_sequence = r.sequence;
                    have_last = true;
                    result.records.push_back(std::move(r));
                }
                pos = end;
                continue;
            }
        }
        if (stream[pos] != std::byte{0}) ++report.bytes_skipped; // нули — дозаполнение полосы, не мусор
        ++pos;
    }
    report.records = result.records.size();
    return result;
}

std::expected<RecoveryReport, std::string> verify(Storage& storage) {
    auto r = read_all(storage, false);
    if (!r) return std::unexpected(r.error());
    return std::move(r->report);
}

std::expected<RecoveryReport, std::string> repair(Storage& storage) {
    auto r = read_all(storage, true);
    if (!r) return std::unexpected(r.error());
    return std::move(r->report);
}

// ------------------------------------------------------------------------------------------------ запись

Writer::Writer(Storage& storage, const Config& config, std::uint64_t stripe, std::uint64_t sequence)
    : m_storage(&storage), m_config(config), m_code(config.data_blocks, config.parity_blocks), m_stripe(stripe), m_sequence(sequence) {}

std::expected<Writer, std::string> Writer::create(Storage& storage, const Config& config) {
    if (auto ok = validate(config); !ok) return std::unexpected(ok.error());
    if (!storage.truncate(0)) return std::unexpected("журнал: хранилище не очищается");
    const auto header = make_header(config);
    if (!storage.write(0, header) || !storage.write(header_bytes, header) || !storage.flush()) return std::unexpected("журнал: заголовок не записывается");
    return Writer(storage, config, 0, 0);
}

std::expected<Writer, std::string> Writer::resume(Storage& storage) {
    auto read = read_all(storage, /*repair=*/true); // чиним, прежде чем дописывать
    if (!read) return std::unexpected(read.error());
    std::array<std::byte, header_bytes> head{};
    (void)storage.read(0, head);
    auto config = parse_header(head);
    if (!config) {
        (void)storage.read(header_bytes, head);
        config = parse_header(head);
    }
    const std::uint64_t sequence = read->records.empty() ? 0 : read->records.back().sequence + 1;
    return Writer(storage, *config, read->report.stripes, sequence);
}

Writer::~Writer() {
    if (m_storage != nullptr && !m_pending.empty()) (void)flush();
}

std::uint64_t Writer::append(std::uint32_t tick, std::uint32_t type, std::span<const std::byte> payload) {
    FLUX_ASSERT(payload.size() <= max_payload, "EventLog::Writer::append: событие больше 1 МиБ");
    const std::uint64_t sequence = m_sequence++;
    const std::size_t at = m_pending.size();
    m_pending.resize(at + record_header + payload.size() + record_trailer);
    std::byte* r = m_pending.data() + at;
    r[0] = sync0;
    r[1] = sync1;
    put<std::uint32_t>(r + 2, static_cast<std::uint32_t>(payload.size()));
    put<std::uint64_t>(r + 6, sequence);
    put<std::uint32_t>(r + 14, tick);
    put<std::uint32_t>(r + 18, type);
    if (!payload.empty()) std::memcpy(r + record_header, payload.data(), payload.size());
    put<std::uint32_t>(r + record_header + payload.size(), Math::crc32c(std::span<const std::byte>(r, record_header + payload.size())));
    while (m_pending.size() >= static_cast<std::size_t>(m_config.data_blocks) * m_config.block_size) emit_stripe();
    return sequence;
}

bool Writer::emit_stripe() {
    const std::size_t bs = m_config.block_size, k = m_config.data_blocks, m = m_config.parity_blocks;
    const std::size_t data_bytes = k * bs;
    m_pending.resize(std::max(m_pending.size(), data_bytes), std::byte{0}); // flush: добивка нулями до целой полосы
    std::vector<std::vector<std::byte>> parity(m, std::vector<std::byte>(bs));
    std::vector<std::span<const std::byte>> data;
    std::vector<std::span<std::byte>> parity_spans;
    for (std::size_t i = 0; i < k; ++i) data.emplace_back(m_pending.data() + i * bs, bs);
    for (auto& p : parity) parity_spans.emplace_back(p);
    m_code.encode(data, parity_spans);

    const std::size_t wire = wire_size(m_config);
    std::vector<std::byte> stripe(stripe_bytes(m_config));
    for (std::size_t b = 0; b < k + m; ++b) write_block(stripe.data() + b * wire, m_config, m_stripe, static_cast<int>(b), b < k ? data[b] : std::span<const std::byte>(parity[b - k]));
    const bool ok = m_storage->write(base_offset + m_stripe * stripe.size(), stripe);
    ++m_stripe;
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(data_bytes));
    return ok;
}

bool Writer::flush() {
    bool ok = true;
    if (!m_pending.empty()) ok = emit_stripe();
    return m_storage->flush() && ok;
}

} // namespace EventLog
