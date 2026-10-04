#include <Net/Wire.hpp>

#include <Math/Crc32c.hpp>

#include <cstring>

namespace Net {

namespace {

constexpr std::uint8_t magic0 = 'F', magic1 = 'X';
enum class Kind : std::uint8_t { Hello = 1, Inputs = 2, BlobChunk = 3, BlobAck = 4, HashReport = 5 };

struct Out {
    std::vector<std::byte> data;
    void u8(std::uint8_t v) { data.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) { for (int i = 0; i < 2; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
};

struct In {
    std::span<const std::byte> data;
    std::size_t at = 0;
    bool ok = true;
    std::uint64_t raw(std::size_t bytes) {
        if (!ok || at + bytes > data.size()) { ok = false; return 0; }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[at++])) << (8 * i);
        return v;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(raw(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(raw(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(raw(4)); }
    std::uint64_t u64() { return raw(8); }
    [[nodiscard]] std::size_t left() const { return ok ? data.size() - at : 0; }
};

void put_command(Out& o, const Replay::Command& c) {
    o.u16(c.type);
    o.u16(static_cast<std::uint16_t>(c.arg));
    o.u32(static_cast<std::uint32_t>(c.x));
    o.u32(static_cast<std::uint32_t>(c.y));
    o.u32(static_cast<std::uint32_t>(c.z));
}
Replay::Command get_command(In& in) {
    Replay::Command c;
    c.type = in.u16();
    c.arg = static_cast<std::int16_t>(in.u16());
    c.x = static_cast<std::int32_t>(in.u32());
    c.y = static_cast<std::int32_t>(in.u32());
    c.z = static_cast<std::int32_t>(in.u32());
    return c;
}

} // namespace

std::vector<std::byte> encode(const Packet& packet) {
    Out o;
    o.u8(magic0);
    o.u8(magic1);
    o.u8(protocol_version);
    Kind kind{};
    std::visit([&kind](const auto& b) {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, Hello>) kind = Kind::Hello;
        else if constexpr (std::is_same_v<T, Inputs>) kind = Kind::Inputs;
        else if constexpr (std::is_same_v<T, BlobChunk>) kind = Kind::BlobChunk;
        else if constexpr (std::is_same_v<T, BlobAck>) kind = Kind::BlobAck;
        else kind = Kind::HashReport;
    }, packet.body);
    o.u8(static_cast<std::uint8_t>(kind));
    o.u8(packet.from);
    std::visit([&o](const auto& b) {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, Hello>) {
            o.u8(b.peer_count);
            o.u64(b.seed);
            o.u64(b.config_hash);
            o.u8(b.knows_you ? 1 : 0);
        } else if constexpr (std::is_same_v<T, Inputs>) {
            o.u32(b.acked);
            o.u16(static_cast<std::uint16_t>(b.ticks.size()));
            for (const TickInputs& t : b.ticks) {
                o.u32(t.tick);
                o.u8(static_cast<std::uint8_t>(t.commands.size()));
                for (const Replay::Command& c : t.commands) put_command(o, c);
            }
        } else if constexpr (std::is_same_v<T, BlobChunk>) {
            o.u64(b.hash);
            o.u32(b.total);
            o.u32(b.offset);
            o.u16(static_cast<std::uint16_t>(b.data.size()));
            o.data.insert(o.data.end(), b.data.begin(), b.data.end());
        } else if constexpr (std::is_same_v<T, BlobAck>) {
            o.u64(b.hash);
        } else {
            o.u32(b.tick);
            o.u8(static_cast<std::uint8_t>(b.hashes.count()));
            for (const Replay::NamedHash& h : b.hashes.entries()) {
                for (const char ch : h.name) o.u8(static_cast<std::uint8_t>(ch));
                o.u64(h.value);
            }
        }
    }, packet.body);
    o.u32(Math::crc32c(o.data));
    return std::move(o.data);
}

std::expected<Packet, std::string> decode(std::span<const std::byte> bytes) {
    const auto fail = [](const char* text) { return std::unexpected(std::string(text)); };
    if (bytes.size() < 5 + 4) return fail("пакет короче заголовка");
    if (bytes.size() > max_packet_size) return fail("пакет больше допустимого");
    const std::span<const std::byte> body_bytes = bytes.first(bytes.size() - 4);
    In crc_in{bytes.last(4)};
    if (crc_in.u32() != Math::crc32c(body_bytes)) return fail("контрольная сумма не сходится");

    In in{body_bytes};
    if (in.u8() != magic0 || in.u8() != magic1) return fail("не пакет FluxEng");
    if (in.u8() != protocol_version) return fail("другая версия протокола");
    const auto kind = static_cast<Kind>(in.u8());
    Packet packet;
    packet.from = in.u8();

    switch (kind) {
    case Kind::Hello: {
        Hello h;
        h.peer_count = in.u8();
        h.seed = in.u64();
        h.config_hash = in.u64();
        h.knows_you = in.u8() != 0;
        if (h.peer_count == 0) return fail("Hello: ноль пиров");
        packet.body = h;
        break;
    }
    case Kind::Inputs: {
        Inputs m;
        m.acked = in.u32();
        const std::uint16_t count = in.u16();
        for (std::uint16_t i = 0; i < count && in.ok; ++i) {
            TickInputs t;
            t.tick = in.u32();
            const std::uint8_t n = in.u8();
            if (n > max_commands_per_tick) return fail("Inputs: слишком много команд в тике");
            if (in.left() < static_cast<std::size_t>(n) * sizeof(Replay::Command)) return fail("Inputs: пакет обрезан");
            for (std::uint8_t k = 0; k < n; ++k) t.commands.push_back(get_command(in));
            m.ticks.push_back(std::move(t));
        }
        packet.body = std::move(m);
        break;
    }
    case Kind::BlobChunk: {
        BlobChunk c;
        c.hash = in.u64();
        c.total = in.u32();
        c.offset = in.u32();
        const std::uint16_t len = in.u16();
        if (!in.ok || in.left() < len) return fail("BlobChunk: пакет обрезан");
        if (c.total == 0 || c.total > max_blob_size) return fail("BlobChunk: недопустимый размер блоба");
        if (static_cast<std::uint64_t>(c.offset) + len > c.total) return fail("BlobChunk: кусок за пределами блоба");
        c.data.assign(body_bytes.begin() + static_cast<std::ptrdiff_t>(in.at), body_bytes.begin() + static_cast<std::ptrdiff_t>(in.at + len));
        in.at += len;
        packet.body = std::move(c);
        break;
    }
    case Kind::BlobAck: packet.body = BlobAck{in.u64()}; break;
    case Kind::HashReport: {
        HashReport r;
        r.tick = in.u32();
        const std::uint8_t count = in.u8();
        if (count > Replay::StateHashes::capacity) return fail("HashReport: слишком много хешей");
        for (std::uint8_t i = 0; i < count && in.ok; ++i) {
            char name[16];
            for (char& ch : name) ch = static_cast<char>(in.u8());
            name[15] = '\0';
            const std::uint64_t value = in.u64();
            if (!in.ok || name[0] == '\0' || r.hashes.find(name)) return fail("HashReport: плохое имя подсистемы");
            r.hashes.add(name, value);
        }
        packet.body = std::move(r);
        break;
    }
    default: return fail("неизвестный вид пакета");
    }
    if (!in.ok) return fail("пакет обрезан");
    if (in.left() != 0) return fail("в пакете лишние байты");
    return packet;
}

} // namespace Net
