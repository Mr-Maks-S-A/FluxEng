#pragma once
/**
 * @file Rng.hpp
 * @brief Rng — генератор с сидом (SplitMix64): единственный источник случайности симуляции.
 */

#include <Math/Fixed.hpp>

#include <cstdint>

namespace Math {

/// @brief Перемешивание 64 бит (финализатор SplitMix64): основа шума и хеширования координат.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed = 0) noexcept : m_state(seed) {}

    [[nodiscard]] constexpr std::uint64_t next() noexcept { return mix64(m_state += 0x9E3779B97F4A7C15ULL); }
    [[nodiscard]] constexpr std::uint32_t next_u32() noexcept { return static_cast<std::uint32_t>(next() >> 32); }
    /// @brief Число в [0, bound); bound = 0 даёт 0.
    [[nodiscard]] constexpr std::uint32_t below(std::uint32_t bound) noexcept {
        return bound == 0 ? 0 : static_cast<std::uint32_t>((next() >> 32) * bound >> 32);
    }
    /// @brief Fixed в [0, 1).
    [[nodiscard]] constexpr Fixed unit() noexcept { return Fixed::from_raw(static_cast<std::int32_t>(next() >> 48)); }

    [[nodiscard]] constexpr std::uint64_t state() const noexcept { return m_state; }

private:
    std::uint64_t m_state;
};

} // namespace Math
