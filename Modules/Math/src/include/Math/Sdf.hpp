#pragma once
/**
 * @file Sdf.hpp
 * @brief Знаковое поле расстояний как интерфейс: всё, что нужно движению и лучам, чтобы работать с любой поверхностью.
 *
 * `sample` < 0 — внутри твёрдого тела, > 0 — снаружи, 0 — поверхность. Ландшафт (`Terrain::World`), аналитические
 * плоскости и шары, а позже планеты и корабли реализуют один и тот же интерфейс — персонаж и заклинания не знают,
 * по чему идут. Аналитические поля (`PlaneSdf`, `SphereSdf`) делают тесты движения мгновенными.
 */

#include <Math/Fixed.hpp>
#include <Math/Vec.hpp>

#include <optional>

namespace Math {

class SdfField {
public:
    virtual ~SdfField() = default;
    /// @brief Расстояние до поверхности в метрах (со знаком).
    [[nodiscard]] virtual Fixed sample(WorldPos pos) const noexcept = 0;
    /// @brief Единичная нормаль «наружу». По умолчанию — центральные разности по `sample` с шагом 25 см.
    [[nodiscard]] virtual FVec3 gradient(WorldPos pos) const noexcept;
};

struct RayHit {
    WorldPos position;
    Fixed distance; ///< Длина луча до точки.
};

/// @brief Луч по длине шага из расстояния; `dir` — единичный. `nullopt`, если до `max` ничего нет.
[[nodiscard]] std::optional<RayHit> raycast(const SdfField& field, WorldPos origin, FVec3 dir, Fixed max) noexcept;

/// @brief Полупространство ниже плоскости y = `height` (в WorldPos): плоский мир.
struct PlaneSdf final : SdfField {
    std::int64_t height = 0;
    explicit PlaneSdf(std::int64_t height_raw = 0) : height(height_raw) {}
    [[nodiscard]] Fixed sample(WorldPos pos) const noexcept override { return Fixed::saturate(pos.y - height); }
    [[nodiscard]] FVec3 gradient(WorldPos) const noexcept override { return {Fixed{}, Fixed::from_int(1), Fixed{}}; }
};

/// @brief Твёрдый шар: «планета» для проверок.
struct SphereSdf final : SdfField {
    WorldPos centre;
    Fixed radius;
    SphereSdf(WorldPos c, Fixed r) : centre(c), radius(r) {}
    [[nodiscard]] Fixed sample(WorldPos pos) const noexcept override;
};

} // namespace Math
