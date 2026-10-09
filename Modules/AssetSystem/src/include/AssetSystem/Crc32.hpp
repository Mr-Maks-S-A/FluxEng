#pragma once
/**
 * @file Crc32.hpp
 * @brief CRC-32 (IEEE, полином 0xEDB88320): контрольная сумма записей пака и бинарных сеток.
 *
 * Slicing-by-8: по 8 байт за шаг на 8 таблицах (в 5–8 раз быстрее побайтового варианта; результат тот же).
 * Побайтовая CRC съедала бы большую часть времени разбора `.fmesh`.
 */

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace AssetSystem {

namespace detail {
constexpr std::array<std::array<std::uint32_t, 256>, 8> make_crc_tables() noexcept {
    std::array<std::array<std::uint32_t, 256>, 8> tables{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        tables[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
        for (std::size_t t = 1; t < 8; ++t) tables[t][i] = (tables[t - 1][i] >> 8) ^ tables[0][tables[t - 1][i] & 0xFFu];
    }
    return tables;
}
inline constexpr auto crc_tables = make_crc_tables();
} // namespace detail

[[nodiscard]] inline std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t crc = 0) noexcept {
    static_assert(std::endian::native == std::endian::little);
    const auto& t = detail::crc_tables;
    crc = ~crc;
    const std::byte* p = data.data();
    std::size_t n = data.size();
    while (n >= 8) {
        std::uint64_t chunk = 0;
        std::memcpy(&chunk, p, 8);
        chunk ^= crc;
        crc = t[7][chunk & 0xFF] ^ t[6][(chunk >> 8) & 0xFF] ^ t[5][(chunk >> 16) & 0xFF] ^ t[4][(chunk >> 24) & 0xFF] ^
              t[3][(chunk >> 32) & 0xFF] ^ t[2][(chunk >> 40) & 0xFF] ^ t[1][(chunk >> 48) & 0xFF] ^ t[0][chunk >> 56];
        p += 8;
        n -= 8;
    }
    while (n-- > 0) crc = t[0][(crc ^ std::to_integer<std::uint32_t>(*p++)) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

} // namespace AssetSystem
