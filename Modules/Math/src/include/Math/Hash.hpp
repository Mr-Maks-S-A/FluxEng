#pragma once
/**
 * @file Hash.hpp
 * @brief Hasher — хеш состояния (FNV-1a, 64 бита): сравнение миров, записей и повторов.
 */

#include <cstddef>
#include <cstdint>
#include <span>

namespace Math {

class Hasher {
public:
    static constexpr std::uint64_t offset_basis = 14695981039346656037ULL;

    constexpr void add_byte(std::uint8_t byte) noexcept { m_hash = (m_hash ^ byte) * 1099511628211ULL; }
    /// @brief Добавляет целое по байтам (порядок little-endian: хеш одинаков на любой платформе).
    constexpr void add(std::uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i) add_byte(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    constexpr void add_signed(std::int64_t value) noexcept { add(static_cast<std::uint64_t>(value)); }
    void add_bytes(std::span<const std::byte> bytes) noexcept {
        for (const std::byte b : bytes) add_byte(static_cast<std::uint8_t>(b));
    }
    /// @brief Хеширует массив тривиальных значений как байты (для блоков данных: ландшафт, поле маны).
    template<typename T>
    void add_span(std::span<const T> values) noexcept {
        add_bytes(std::as_bytes(values));
    }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return m_hash; }

private:
    std::uint64_t m_hash = offset_basis;
};

/// @brief Хеш содержимого байтов вместе с длиной: имя блоба в записи и сети (одинаковый везде, где нужен «адрес по содержимому»).
[[nodiscard]] inline std::uint64_t content_hash(std::span<const std::byte> bytes) noexcept {
    Hasher h;
    h.add(bytes.size());
    h.add_bytes(bytes);
    return h.value();
}

} // namespace Math
