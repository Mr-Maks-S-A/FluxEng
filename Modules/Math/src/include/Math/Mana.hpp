#pragma once
/**
 * @file Mana.hpp
 * @brief Mana — количество маны. Тот же Fixed внутри, но другой тип: ману нельзя случайно сложить с метрами,
 * радиусом или временем, а передать как радиус заклинания — ошибка компиляции.
 *
 * Допустимо: Mana ± Mana, Mana · Fixed (масштаб), Mana / Fixed, Mana / Mana (отношение → Fixed), сравнения.
 * Недопустимо: Mana + Fixed, Mana · Mana. Переход к числу — явный (`value`, `raw()`).
 */

#include <Math/Fixed.hpp>

namespace Math {

struct Mana {
    Fixed value{};

    constexpr Mana() = default;
    constexpr explicit Mana(Fixed v) noexcept : value(v) {}
    [[nodiscard]] static constexpr Mana from_int(std::int32_t v) noexcept { return Mana(Fixed::from_int(v)); }
    [[nodiscard]] static constexpr Mana from_raw(std::int32_t raw) noexcept { return Mana(Fixed::from_raw(raw)); }
    [[nodiscard]] static constexpr Mana from_ratio(std::int64_t num, std::int64_t den) noexcept { return Mana(Fixed::from_ratio(num, den)); }
    [[nodiscard]] static constexpr Mana max() noexcept { return Mana(Fixed::max()); }

    [[nodiscard]] constexpr std::int32_t raw() const noexcept { return value.raw; }
    /// @brief Только для отображения и тестов.
    [[nodiscard]] constexpr double to_double() const noexcept { return value.to_double(); }

    [[nodiscard]] constexpr Mana operator-() const noexcept { return Mana(-value); }
    [[nodiscard]] friend constexpr Mana operator+(Mana a, Mana b) noexcept { return Mana(a.value + b.value); }
    [[nodiscard]] friend constexpr Mana operator-(Mana a, Mana b) noexcept { return Mana(a.value - b.value); }
    [[nodiscard]] friend constexpr Mana operator*(Mana a, Fixed s) noexcept { return Mana(a.value * s); }
    [[nodiscard]] friend constexpr Mana operator*(Fixed s, Mana a) noexcept { return Mana(a.value * s); }
    [[nodiscard]] friend constexpr Mana operator/(Mana a, Fixed s) noexcept { return Mana(a.value / s); }
    /// @brief Отношение двух количеств маны — безразмерное число.
    [[nodiscard]] friend constexpr Fixed operator/(Mana a, Mana b) noexcept { return a.value / b.value; }
    constexpr Mana& operator+=(Mana o) noexcept { return *this = *this + o; }
    constexpr Mana& operator-=(Mana o) noexcept { return *this = *this - o; }

    [[nodiscard]] friend constexpr auto operator<=>(Mana, Mana) noexcept = default;
};
static_assert(sizeof(Mana) == sizeof(Fixed));

[[nodiscard]] constexpr Mana min(Mana a, Mana b) noexcept { return a < b ? a : b; }
[[nodiscard]] constexpr Mana max(Mana a, Mana b) noexcept { return a < b ? b : a; }
[[nodiscard]] constexpr Mana clamp(Mana v, Mana lo, Mana hi) noexcept { return v < lo ? lo : v > hi ? hi : v; }

namespace literals {
/// @brief `240_mana`, `0.05_mana` — константы маны при компиляции.
[[nodiscard]] consteval Mana operator""_mana(long double v) { return Mana(operator""_fx(v)); }
[[nodiscard]] consteval Mana operator""_mana(unsigned long long v) { return Mana(operator""_fx(v)); }
} // namespace literals

} // namespace Math
