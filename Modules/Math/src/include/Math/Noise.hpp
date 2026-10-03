#pragma once
/**
 * @file Noise.hpp
 * @brief Шум с сидом на Fixed: значения решётки из хеша, сглаженная интерполяция. Без float.
 */

#include <Math/Fixed.hpp>

#include <cstdint>

namespace Math {

/// @brief Значение решётки (x, z) в [0, 1).
[[nodiscard]] Fixed lattice_value(std::uint64_t seed, std::int32_t x, std::int32_t z) noexcept;
/// @brief Value-noise в [0, 1); (x, z) — в клетках решётки.
[[nodiscard]] Fixed value_noise(std::uint64_t seed, Fixed x, Fixed z) noexcept;
/// @brief Сумма октав: амплитуда каждой следующей вдвое меньше, частота вдвое выше; результат в [0, 1).
[[nodiscard]] Fixed fbm(std::uint64_t seed, Fixed x, Fixed z, int octaves) noexcept;

} // namespace Math
