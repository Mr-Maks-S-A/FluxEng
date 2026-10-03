#pragma once
/**
 * @file Vec.hpp
 * @brief FVec3 — вектор из Fixed; WorldPos — позиция в мире на int64.
 *
 * WorldPos: единицы 1/65536 м, диапазон ≈ ±1,4·10¹⁴ м (около 940 а.е.) при точности 15 мкм —
 * Солнечная система помещается без смены формата. Fixed ↔ WorldPos связаны одной шкалой Q16.16.
 */

#include <Math/Fixed.hpp>

#include <array>
#include <cstdint>

namespace Math {

struct FVec3 {
    Fixed x{}, y{}, z{};

    [[nodiscard]] friend constexpr FVec3 operator+(FVec3 a, FVec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    [[nodiscard]] friend constexpr FVec3 operator-(FVec3 a, FVec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    [[nodiscard]] friend constexpr FVec3 operator*(FVec3 a, Fixed s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
    [[nodiscard]] friend constexpr FVec3 operator*(Fixed s, FVec3 a) noexcept { return a * s; }
    [[nodiscard]] constexpr FVec3 operator-() const noexcept { return {-x, -y, -z}; }
    [[nodiscard]] friend constexpr bool operator==(FVec3, FVec3) noexcept = default;
};

/// @brief Скалярное произведение с насыщением.
[[nodiscard]] constexpr Fixed dot(FVec3 a, FVec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr FVec3 cross(FVec3 a, FVec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
/// @brief Длина (точная: корень из суммы квадратов в int64, без переполнения Fixed).
[[nodiscard]] Fixed length(FVec3 v) noexcept;
/// @brief Единичный вектор; нулевой остаётся нулевым.
[[nodiscard]] FVec3 normalize(FVec3 v) noexcept;

struct WorldPos {
    std::int64_t x = 0, y = 0, z = 0; ///< 1/65536 м.

    [[nodiscard]] static constexpr WorldPos from_meters(std::int64_t mx, std::int64_t my, std::int64_t mz) noexcept {
        return {mx * Fixed::one_raw, my * Fixed::one_raw, mz * Fixed::one_raw};
    }
    /**
     * @brief Граница с float (ввод, камера, отображение): метры double → WorldPos с округлением.
     * Симуляция эту функцию не вызывает.
     */
    [[nodiscard]] static constexpr WorldPos from_doubles(double mx, double my, double mz) noexcept {
        const auto conv = [](double m) {
            const double scaled = m * static_cast<double>(Fixed::one_raw);
            return static_cast<std::int64_t>(scaled + (scaled >= 0 ? 0.5 : -0.5));
        };
        return {conv(mx), conv(my), conv(mz)};
    }
    /// @brief Метры как double — только для отображения и ввода.
    [[nodiscard]] constexpr std::array<double, 3> to_doubles() const noexcept {
        const auto conv = [](std::int64_t v) { return static_cast<double>(v) / static_cast<double>(Fixed::one_raw); };
        return {conv(x), conv(y), conv(z)};
    }
    [[nodiscard]] friend constexpr WorldPos operator+(WorldPos a, WorldPos b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    [[nodiscard]] friend constexpr WorldPos operator-(WorldPos a, WorldPos b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    [[nodiscard]] friend constexpr bool operator==(WorldPos, WorldPos) noexcept = default;
};

/// @brief Смещение как FVec3 (насыщается за ±32 км — для расстояний «рядом» этого хватает).
[[nodiscard]] constexpr FVec3 to_fvec(WorldPos p) noexcept {
    return {Fixed::saturate(p.x), Fixed::saturate(p.y), Fixed::saturate(p.z)};
}
[[nodiscard]] constexpr WorldPos to_world(FVec3 v) noexcept { return {v.x.raw, v.y.raw, v.z.raw}; }
/// @brief pos + v · scale (scale в Fixed): шаг движения без потери точности int64.
[[nodiscard]] constexpr WorldPos advance(WorldPos p, FVec3 v, Fixed scale) noexcept {
    return {p.x + static_cast<std::int64_t>(v.x.raw) * scale.raw / Fixed::one_raw, p.y + static_cast<std::int64_t>(v.y.raw) * scale.raw / Fixed::one_raw,
            p.z + static_cast<std::int64_t>(v.z.raw) * scale.raw / Fixed::one_raw};
}
/// @brief |a − b|² в Q32.32 (int64 без насыщения): для сравнения расстояний.
[[nodiscard]] constexpr std::int64_t distance_sq_raw(WorldPos a, WorldPos b) noexcept {
    const WorldPos d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}
/// @brief |a − b| в Fixed.
[[nodiscard]] Fixed distance(WorldPos a, WorldPos b) noexcept;

/// @brief Направление из float (взгляд камеры): единичный FVec3. Нулевой вектор остаётся нулевым.
[[nodiscard]] FVec3 quantize_direction(double x, double y, double z) noexcept;

/// @brief Гравитация как функция от позиции. В пре-альфе — константа (0; −9,81; 0); планеты — позже.
[[nodiscard]] constexpr FVec3 gravity(WorldPos) noexcept { return {Fixed{}, Fixed::from_raw(-642908), Fixed{}}; }

} // namespace Math
