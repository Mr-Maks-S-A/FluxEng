#pragma once
/**
 * @file Crc32c.hpp
 * @brief CRC-32C (полином Кастаньоли, 0x1EDC6F41): проверка целостности блоков данных и записей журнала.
 *
 * В отличие от `Hasher` (FNV-1a — хеш состояния, быстрый, без гарантий), CRC обнаруживает **любую** ошибку
 * до 32 бит подряд и все ошибки нечётного числа битов — это то, что нужно при чтении повреждённого файла.
 * Результат одинаков на любой платформе (программная реализация, таблица считается при компиляции).
 */

#include <cstddef>
#include <cstdint>
#include <span>

namespace Math {

/// @brief CRC-32C от байтов. `seed` — результат предыдущего вызова, чтобы считать по частям: `crc32c(b, crc32c(a)) == crc32c(a ++ b)`.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes, std::uint32_t seed = 0) noexcept;

} // namespace Math
