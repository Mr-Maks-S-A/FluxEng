#pragma once
/**
 * @file Fixed.hpp
 * @brief Fixed — число с фиксированной запятой Q16.16 на int32 для скоростей, расстояний и маны.
 *
 * Формат задаётся в одном месте (`Fixed::frac_bits`). Умножение и деление идут через int64 и
 * округляют к нулю, поэтому `a * b` и `(-a) * b` симметричны: потоки маны складываются в ноль.
 * Переполнение результата насыщается (`saturate`), а не оборачивается.
 */

#include <compare>
#include <cstdint>
#include <limits>

namespace Math {

struct Fixed {
    static constexpr int frac_bits = 16;
    static constexpr std::int64_t one_raw = std::int64_t{1} << frac_bits;

    std::int32_t raw = 0;

    constexpr Fixed() = default;

    [[nodiscard]] static constexpr Fixed from_raw(std::int32_t raw) noexcept {
        Fixed f;
        f.raw = raw;
        return f;
    }
    /// @brief Приводит int64 к Fixed с насыщением.
    [[nodiscard]] static constexpr Fixed saturate(std::int64_t raw) noexcept {
        constexpr std::int64_t lo = std::numeric_limits<std::int32_t>::min(), hi = std::numeric_limits<std::int32_t>::max();
        return from_raw(static_cast<std::int32_t>(raw < lo ? lo : raw > hi ? hi : raw));
    }
    [[nodiscard]] static constexpr Fixed from_int(std::int32_t value) noexcept { return saturate(static_cast<std::int64_t>(value) * one_raw); }
    /// @brief num / den, округление к нулю. Для констант: `Fixed::from_ratio(981, 100)`.
    [[nodiscard]] static constexpr Fixed from_ratio(std::int64_t num, std::int64_t den) noexcept { return saturate(num * one_raw / den); }
    [[nodiscard]] static constexpr Fixed max() noexcept { return from_raw(std::numeric_limits<std::int32_t>::max()); }
    [[nodiscard]] static constexpr Fixed min() noexcept { return from_raw(std::numeric_limits<std::int32_t>::min()); }

    /**
     * @brief Граница с float: ввод (мышь, камера) и отображение. Округляет к ближайшему, насыщается.
     * Симуляция этой функции не вызывает — только код, который превращает ввод в команды.
     */
    [[nodiscard]] static constexpr Fixed from_double(double v) noexcept {
        constexpr double limit = 32767.99;
        if (!(v == v)) return {}; // NaN
        v = v > limit ? limit : v < -limit ? -limit : v;
        return from_raw(static_cast<std::int32_t>(v * static_cast<double>(one_raw) + (v >= 0 ? 0.5 : -0.5)));
    }

    /// @brief Целая часть (к минус бесконечности).
    [[nodiscard]] constexpr std::int32_t floor() const noexcept { return raw >> frac_bits; }
    /// @brief Только для отображения и тестов: симуляция это значение не читает.
    [[nodiscard]] constexpr double to_double() const noexcept { return static_cast<double>(raw) / static_cast<double>(one_raw); }

    [[nodiscard]] constexpr Fixed operator-() const noexcept { return saturate(-static_cast<std::int64_t>(raw)); }
    [[nodiscard]] friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept { return saturate(static_cast<std::int64_t>(a.raw) + b.raw); }
    [[nodiscard]] friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept { return saturate(static_cast<std::int64_t>(a.raw) - b.raw); }
    [[nodiscard]] friend constexpr Fixed operator*(Fixed a, Fixed b) noexcept {
        return saturate(static_cast<std::int64_t>(a.raw) * b.raw / one_raw);
    }
    /// @brief Деление; на ноль — насыщение в знак делимого (симуляция не падает).
    [[nodiscard]] friend constexpr Fixed operator/(Fixed a, Fixed b) noexcept {
        if (b.raw == 0) return a.raw >= 0 ? max() : min();
        return saturate(static_cast<std::int64_t>(a.raw) * one_raw / b.raw);
    }
    constexpr Fixed& operator+=(Fixed o) noexcept { return *this = *this + o; }
    constexpr Fixed& operator-=(Fixed o) noexcept { return *this = *this - o; }
    constexpr Fixed& operator*=(Fixed o) noexcept { return *this = *this * o; }

    [[nodiscard]] friend constexpr auto operator<=>(Fixed, Fixed) noexcept = default;
};

[[nodiscard]] constexpr Fixed abs(Fixed a) noexcept { return a.raw < 0 ? -a : a; }
[[nodiscard]] constexpr Fixed min(Fixed a, Fixed b) noexcept { return a < b ? a : b; }
[[nodiscard]] constexpr Fixed max(Fixed a, Fixed b) noexcept { return a < b ? b : a; }
[[nodiscard]] constexpr Fixed clamp(Fixed v, Fixed lo, Fixed hi) noexcept { return v < lo ? lo : v > hi ? hi : v; }
/// @brief a + (b − a) · t.
[[nodiscard]] constexpr Fixed lerp(Fixed a, Fixed b, Fixed t) noexcept { return a + (b - a) * t; }

/// @brief Целочисленный квадратный корень (пол) из uint64.
[[nodiscard]] std::uint64_t isqrt(std::uint64_t value) noexcept;
/// @brief √a для a ≥ 0 (отрицательное — 0). Результат точен до младшего разряда.
[[nodiscard]] Fixed sqrt(Fixed a) noexcept;

namespace literals {
/// @brief `1.5_fx` — константа Fixed из десятичной записи (вычисляется при компиляции).
[[nodiscard]] consteval Fixed operator""_fx(long double v) {
    return Fixed::from_raw(static_cast<std::int32_t>(v * static_cast<long double>(Fixed::one_raw) + (v >= 0 ? 0.5L : -0.5L)));
}
[[nodiscard]] consteval Fixed operator""_fx(unsigned long long v) { return Fixed::from_int(static_cast<std::int32_t>(v)); }
} // namespace literals

} // namespace Math
