#include <InputSystem/InputLog.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <type_traits>

namespace InputSystem {

static_assert(std::endian::native == std::endian::little, "InputLog format is little-endian");

namespace {

constexpr char kMagic[4] = {'F', 'X', 'I', 'L'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaxEvents = std::size_t{1} << 24;

constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}
constexpr auto kCrc = make_crc_table();

std::uint32_t crc32(const std::byte* data, std::size_t size) noexcept {
    std::uint32_t crc = ~0u;
    for (std::size_t i = 0; i < size; ++i) crc = kCrc[(crc ^ std::to_integer<std::uint32_t>(data[i])) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

class Out {
public:
    template<typename T>
    void put(T value) {
        const std::size_t at = m_bytes.size();
        m_bytes.resize(at + sizeof(T));
        std::memcpy(m_bytes.data() + at, &value, sizeof(T));
    }
    std::vector<std::byte>& bytes() noexcept { return m_bytes; }

private:
    std::vector<std::byte> m_bytes;
};

class In {
public:
    explicit In(std::span<const std::byte> data) noexcept : m_data(data) {}
    template<typename T>
    T get() noexcept {
        T value{};
        if (m_data.size() - m_pos < sizeof(T)) {
            m_ok = false;
            return value;
        }
        std::memcpy(&value, m_data.data() + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return value;
    }
    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t position() const noexcept { return m_pos; }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_data.size() - m_pos; }

private:
    std::span<const std::byte> m_data;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

void write_event(Out& out, const InputEvent& event) {
    out.put<std::uint8_t>(static_cast<std::uint8_t>(event.index()));
    std::visit(
        [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, KeyInput>) {
                out.put<std::uint16_t>(static_cast<std::uint16_t>(e.key));
                out.put<std::uint8_t>(static_cast<std::uint8_t>(e.transition));
                out.put<std::uint8_t>(static_cast<std::uint8_t>(e.mods));
            } else if constexpr (std::is_same_v<T, MouseButtonInput>) {
                out.put<std::uint8_t>(static_cast<std::uint8_t>(e.button));
                out.put<std::uint8_t>(static_cast<std::uint8_t>(e.transition));
                out.put<std::uint8_t>(static_cast<std::uint8_t>(e.mods));
            } else if constexpr (std::is_same_v<T, CursorInput>) {
                out.put<double>(e.x);
                out.put<double>(e.y);
            } else if constexpr (std::is_same_v<T, ScrollInput>) {
                out.put<double>(e.dx);
                out.put<double>(e.dy);
            } else if constexpr (std::is_same_v<T, CharInput>) {
                out.put<std::uint32_t>(e.codepoint);
            } else if constexpr (std::is_same_v<T, FocusInput>) {
                out.put<std::uint8_t>(e.focused ? 1 : 0);
            } else if constexpr (std::is_same_v<T, GamepadConnectionInput>) {
                out.put<std::uint8_t>(e.pad);
                out.put<std::uint8_t>(e.connected ? 1 : 0);
            } else if constexpr (std::is_same_v<T, GamepadButtonInput>) {
                out.put<std::uint8_t>(e.pad);
                out.put<std::uint8_t>(static_cast<std::uint8_t>(e.button));
                out.put<std::uint8_t>(static_cast<std::uint8_t>(e.transition));
            } else if constexpr (std::is_same_v<T, GamepadAxisInput>) {
                out.put<std::uint8_t>(e.pad);
                out.put<std::uint8_t>(static_cast<std::uint8_t>(e.axis));
                out.put<float>(e.value);
            }
        },
        event);
}

bool valid_transition(std::uint8_t t) noexcept { return t <= static_cast<std::uint8_t>(Transition::Repeat); }

/// Разбор одного события; nullopt — данные неверны (индекс типа, диапазон значения, не число).
std::optional<InputEvent> read_event(In& in) {
    const auto type = in.get<std::uint8_t>();
    switch (type) {
        case 0: {
            KeyInput e;
            const auto key = in.get<std::uint16_t>();
            const auto transition = in.get<std::uint8_t>();
            const auto mods = in.get<std::uint8_t>();
            if (!in.ok() || !valid_transition(transition) || mods >= 64) return std::nullopt;
            e.key = static_cast<Key>(key);
            e.transition = static_cast<Transition>(transition);
            e.mods = static_cast<Modifiers>(mods);
            return e;
        }
        case 1: {
            MouseButtonInput e;
            const auto button = in.get<std::uint8_t>();
            const auto transition = in.get<std::uint8_t>();
            const auto mods = in.get<std::uint8_t>();
            if (!in.ok() || button >= kMouseButtonCount || !valid_transition(transition) || mods >= 64) return std::nullopt;
            e.button = static_cast<MouseButton>(button);
            e.transition = static_cast<Transition>(transition);
            e.mods = static_cast<Modifiers>(mods);
            return e;
        }
        case 2: {
            CursorInput e;
            e.x = in.get<double>();
            e.y = in.get<double>();
            if (!in.ok() || !std::isfinite(e.x) || !std::isfinite(e.y)) return std::nullopt;
            return e;
        }
        case 3: {
            ScrollInput e;
            e.dx = in.get<double>();
            e.dy = in.get<double>();
            if (!in.ok() || !std::isfinite(e.dx) || !std::isfinite(e.dy)) return std::nullopt;
            return e;
        }
        case 4: {
            CharInput e;
            e.codepoint = in.get<std::uint32_t>();
            if (!in.ok() || e.codepoint > 0x10FFFF) return std::nullopt;
            return e;
        }
        case 5: {
            FocusInput e;
            const auto focused = in.get<std::uint8_t>();
            if (!in.ok() || focused > 1) return std::nullopt;
            e.focused = focused != 0;
            return e;
        }
        case 6: {
            GamepadConnectionInput e;
            e.pad = in.get<std::uint8_t>();
            const auto connected = in.get<std::uint8_t>();
            if (!in.ok() || e.pad >= kMaxGamepads || connected > 1) return std::nullopt;
            e.connected = connected != 0;
            return e;
        }
        case 7: {
            GamepadButtonInput e;
            e.pad = in.get<std::uint8_t>();
            const auto button = in.get<std::uint8_t>();
            const auto transition = in.get<std::uint8_t>();
            if (!in.ok() || e.pad >= kMaxGamepads || button >= kGamepadButtonCount || !valid_transition(transition)) return std::nullopt;
            e.button = static_cast<GamepadButton>(button);
            e.transition = static_cast<Transition>(transition);
            return e;
        }
        case 8: {
            GamepadAxisInput e;
            e.pad = in.get<std::uint8_t>();
            const auto axis = in.get<std::uint8_t>();
            e.value = in.get<float>();
            if (!in.ok() || e.pad >= kMaxGamepads || axis >= kGamepadAxisCount || !std::isfinite(e.value)) return std::nullopt;
            e.axis = static_cast<GamepadAxis>(axis);
            return e;
        }
        default: return std::nullopt;
    }
}

} // namespace

bool InputLog::record(std::uint32_t frame, const InputEvent& event) {
    if (!m_events.empty() && frame < m_events.back().frame) return false;
    m_events.push_back(LoggedEvent{frame, event});
    return true;
}

std::span<const LoggedEvent> InputLog::events_of_frame(std::uint32_t frame) const noexcept {
    const auto first = std::ranges::lower_bound(m_events, frame, {}, &LoggedEvent::frame);
    const auto last = std::ranges::upper_bound(first, m_events.end(), frame, {}, &LoggedEvent::frame);
    return std::span<const LoggedEvent>(m_events).subspan(static_cast<std::size_t>(first - m_events.begin()), static_cast<std::size_t>(last - first));
}

std::vector<std::byte> InputLog::to_bytes() const {
    Out out;
    for (const char c : kMagic) out.put<std::uint8_t>(static_cast<std::uint8_t>(c));
    out.put<std::uint32_t>(kVersion);
    out.put<std::uint32_t>(static_cast<std::uint32_t>(m_events.size()));
    for (const LoggedEvent& logged : m_events) {
        out.put<std::uint32_t>(logged.frame);
        write_event(out, logged.event);
    }
    out.put<std::uint32_t>(crc32(out.bytes().data(), out.bytes().size()));
    return std::move(out.bytes());
}

std::expected<InputLog, std::string> InputLog::from_bytes(std::span<const std::byte> data) {
    if (data.size() < 16) return std::unexpected("input log is shorter than its header");
    if (std::memcmp(data.data(), kMagic, 4) != 0) return std::unexpected("not an input log (bad magic)");
    std::uint32_t stored = 0;
    std::memcpy(&stored, data.data() + data.size() - 4, 4);
    if (crc32(data.data(), data.size() - 4) != stored) return std::unexpected("input log checksum mismatch");

    In in(data.first(data.size() - 4));
    for (int i = 0; i < 4; ++i) (void)in.get<std::uint8_t>();
    const auto version = in.get<std::uint32_t>();
    const auto count = in.get<std::uint32_t>();
    if (version != kVersion) return std::unexpected(std::format("unsupported input log version {}", version));
    if (count > kMaxEvents) return std::unexpected("input log has too many events");

    InputLog log;
    log.m_events.reserve(std::min<std::size_t>(count, in.remaining() / 6)); // не доверяем count при выделении
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto frame = in.get<std::uint32_t>();
        const auto event = read_event(in);
        if (!in.ok() || !event) return std::unexpected(std::format("input log event {} is truncated or invalid", i));
        if (!log.record(frame, *event)) return std::unexpected(std::format("input log event {}: frames go backwards", i));
    }
    if (in.remaining() != 0) return std::unexpected("input log has trailing bytes");
    return log;
}

} // namespace InputSystem
