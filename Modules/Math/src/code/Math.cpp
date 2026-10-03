#include <Math/Math.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace Math {

std::uint64_t isqrt(std::uint64_t value) noexcept {
    if (value == 0) return 0;
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > value) bit >>= 2;
    std::uint64_t result = 0;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

Fixed sqrt(Fixed a) noexcept {
    if (a.raw <= 0) return {};
    // √(raw / 2^16) · 2^16 = √(raw · 2^16)
    return Fixed::from_raw(static_cast<std::int32_t>(isqrt(static_cast<std::uint64_t>(a.raw) << Fixed::frac_bits)));
}

Fixed length(FVec3 v) noexcept {
    const std::int64_t sum = static_cast<std::int64_t>(v.x.raw) * v.x.raw + static_cast<std::int64_t>(v.y.raw) * v.y.raw +
                             static_cast<std::int64_t>(v.z.raw) * v.z.raw;
    return Fixed::saturate(static_cast<std::int64_t>(isqrt(static_cast<std::uint64_t>(sum))));
}

FVec3 normalize(FVec3 v) noexcept {
    const std::int64_t len = length(v).raw;
    if (len == 0) return {};
    const auto div = [&](Fixed c) { return Fixed::saturate(static_cast<std::int64_t>(c.raw) * Fixed::one_raw / len); };
    return {div(v.x), div(v.y), div(v.z)};
}

FVec3 quantize_direction(double x, double y, double z) noexcept {
    return normalize({Fixed::from_double(x), Fixed::from_double(y), Fixed::from_double(z)});
}

Fixed distance(WorldPos a, WorldPos b) noexcept {
    return Fixed::saturate(static_cast<std::int64_t>(isqrt(static_cast<std::uint64_t>(distance_sq_raw(a, b)))));
}

Fixed lattice_value(std::uint64_t seed, std::int32_t x, std::int32_t z) noexcept {
    const std::uint64_t h = mix64(seed ^ mix64(static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) |
                                                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) << 32)));
    return Fixed::from_raw(static_cast<std::int32_t>(h >> 48));
}

namespace {
constexpr Fixed smooth(Fixed t) noexcept { return t * t * (Fixed::from_int(3) - Fixed::from_int(2) * t); }
} // namespace

Fixed value_noise(std::uint64_t seed, Fixed x, Fixed z) noexcept {
    const std::int32_t x0 = x.floor(), z0 = z.floor();
    const Fixed tx = smooth(Fixed::from_raw(x.raw & 0xFFFF)), tz = smooth(Fixed::from_raw(z.raw & 0xFFFF));
    const Fixed a = lerp(lattice_value(seed, x0, z0), lattice_value(seed, x0 + 1, z0), tx);
    const Fixed b = lerp(lattice_value(seed, x0, z0 + 1), lattice_value(seed, x0 + 1, z0 + 1), tx);
    return lerp(a, b, tz);
}

Fixed fbm(std::uint64_t seed, Fixed x, Fixed z, int octaves) noexcept {
    Fixed sum{}, amplitude = Fixed::from_ratio(1, 2), norm{};
    for (int i = 0; i < octaves; ++i) {
        sum += value_noise(seed + static_cast<std::uint64_t>(i) * std::uint64_t{0x9E37}, x, z) * amplitude;
        norm += amplitude;
        amplitude = amplitude * Fixed::from_ratio(1, 2);
        x = x * Fixed::from_int(2);
        z = z * Fixed::from_int(2);
    }
    return norm.raw == 0 ? Fixed{} : min(sum / norm, Fixed::from_raw(0xFFFF));
}

namespace {
AssertHandler g_handler = nullptr;
}

AssertHandler set_assert_handler(AssertHandler handler) noexcept {
    const AssertHandler old = g_handler;
    g_handler = handler;
    return old;
}

void assert_failed(const char* expression, const char* message, const char* file, int line) noexcept {
    std::fprintf(stderr, "FLUX_ASSERT(%s) failed: %s (%s:%d)\n", expression, message, file, line);
    if (g_handler) g_handler(expression, message, file, line);
    std::abort();
}

} // namespace Math

namespace Math {

FVec3 SdfField::gradient(WorldPos pos) const noexcept {
    constexpr std::int64_t h = Fixed::one_raw / 4; // 25 см
    const auto diff = [&](std::int64_t dx, std::int64_t dy, std::int64_t dz) {
        return static_cast<std::int64_t>(sample({pos.x + dx, pos.y + dy, pos.z + dz}).raw) - sample({pos.x - dx, pos.y - dy, pos.z - dz}).raw;
    };
    return normalize({Fixed::saturate(diff(h, 0, 0)), Fixed::saturate(diff(0, h, 0)), Fixed::saturate(diff(0, 0, h))});
}

std::optional<RayHit> raycast(const SdfField& field, WorldPos origin, FVec3 dir, Fixed max) noexcept {
    constexpr std::int64_t epsilon = 1311;  // 2 см
    constexpr std::int64_t min_step = 3277; // 5 см: луч не залипает на касательных
    std::int64_t t = 0;
    for (int i = 0; i < 256 && t <= max.raw; ++i) {
        const WorldPos p = advance(origin, dir, Fixed::saturate(t));
        const std::int64_t d = field.sample(p).raw;
        if (d <= epsilon) return RayHit{p, Fixed::saturate(t)};
        t += std::max(d * 4 / 5, min_step); // 0,8 d: запас на ошибку интерполяции и правок
    }
    return std::nullopt;
}

Fixed SphereSdf::sample(WorldPos pos) const noexcept { return distance(pos, centre) - radius; }

} // namespace Math
