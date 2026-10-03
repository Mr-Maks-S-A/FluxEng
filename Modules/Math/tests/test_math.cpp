#include <Math/Math.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>
#include <set>

using namespace Math;
using namespace Math::literals;

TEST_CASE("Fixed: арифметика Q16.16") {
    CHECK((1.5_fx + 2.25_fx).raw == Fixed::from_ratio(15, 4).raw);
    CHECK((3_fx * 2.5_fx) == 7.5_fx);
    CHECK((7.5_fx / 2.5_fx) == 3_fx);
    CHECK((-3_fx * 2.5_fx) == -(3_fx * 2.5_fx)); // округление к нулю симметрично
    CHECK(Fixed::from_int(5).floor() == 5);
    CHECK((-0.5_fx).floor() == -1);
}

TEST_CASE("Fixed: насыщение и деление на ноль не падают") {
    CHECK(Fixed::max() + 1_fx == Fixed::max());
    CHECK(Fixed::min() - 1_fx == Fixed::min());
    CHECK(Fixed::from_int(30000) * Fixed::from_int(30000) == Fixed::max());
    CHECK(1_fx / Fixed{} == Fixed::max());
    CHECK(-1_fx / Fixed{} == Fixed::min());
}

TEST_CASE("isqrt: пол корня") {
    CHECK(isqrt(0) == 0);
    CHECK(isqrt(1) == 1);
    CHECK(isqrt(15) == 3);
    CHECK(isqrt(16) == 4);
    CHECK(isqrt(~std::uint64_t{0}) == 0xFFFFFFFFULL);
    Rng rng(7);
    for (int i = 0; i < 10000; ++i) {
        const std::uint64_t v = rng.next();
        const std::uint64_t r = isqrt(v);
        CHECK(r * r <= v);
        CHECK((r + 1) * (r + 1) > v); // r+1 ≤ 2^32: произведение не переполняет, кроме верхней границы
    }
}

TEST_CASE("sqrt(Fixed) близок к std::sqrt") {
    for (const double v : {0.25, 1.0, 2.0, 100.0, 1234.5}) {
        const Fixed f = Fixed::from_raw(static_cast<std::int32_t>(v * 65536.0));
        CHECK(std::abs(sqrt(f).to_double() - std::sqrt(v)) < 2.0 / 65536.0);
    }
    CHECK(sqrt(-1_fx) == Fixed{});
}

TEST_CASE("FVec3: длина и нормализация") {
    const FVec3 v{3_fx, 4_fx, 0_fx};
    CHECK(length(v) == 5_fx);
    const FVec3 n = normalize(v);
    CHECK(std::abs(n.x.to_double() - 0.6) < 1e-3);
    CHECK(normalize(FVec3{}) == FVec3{});
}

TEST_CASE("WorldPos: гравитация и шаг движения") {
    CHECK(std::abs(gravity({}).y.to_double() + 9.81) < 1e-4);
    const WorldPos p = advance(WorldPos::from_meters(1, 2, 3), FVec3{2_fx, 0_fx, -1_fx}, 0.5_fx);
    CHECK(p == WorldPos::from_meters(2, 2, 3) - WorldPos{0, 0, Fixed::one_raw / 2});
    // Диапазон: 900 а.е. ≈ 1,35·10¹⁴ м помещается без переполнения int64.
    CHECK(WorldPos::from_meters(134'000'000'000'000LL, 0, 0).x > 0);
}

TEST_CASE("Rng: один сид — одна последовательность") {
    Rng a(42), b(42), c(43);
    for (int i = 0; i < 100; ++i) CHECK(a.next() == b.next());
    CHECK(Rng(42).next() != c.next());
    Rng r(1);
    std::set<std::uint32_t> seen;
    for (int i = 0; i < 1000; ++i) {
        const std::uint32_t v = r.below(10);
        CHECK(v < 10);
        seen.insert(v);
        CHECK(r.unit() < 1_fx);
    }
    CHECK(seen.size() == 10);
}

TEST_CASE("Hasher: стабильное значение и чувствительность к данным") {
    Hasher a, b;
    a.add(1), a.add(2);
    b.add(1), b.add(2);
    CHECK(a.value() == b.value());
    b.add(3);
    CHECK(a.value() != b.value());
    Hasher empty;
    CHECK(empty.value() == Hasher::offset_basis);
}

TEST_CASE("Noise: детерминирован, в диапазоне и непрерывен") {
    for (int i = 0; i < 200; ++i) {
        const Fixed x = Fixed::from_raw(i * 7919), z = Fixed::from_raw(i * 104729);
        CHECK(fbm(5, x, z, 4) == fbm(5, x, z, 4));
        CHECK(fbm(5, x, z, 4) >= Fixed{});
        CHECK(fbm(5, x, z, 4) < 1_fx);
    }
    CHECK(fbm(5, 3.3_fx, 4.1_fx, 4) != fbm(6, 3.3_fx, 4.1_fx, 4));
    const Fixed a = value_noise(1, 2.5_fx, 2.5_fx), b = value_noise(1, Fixed::from_raw(Fixed::from_ratio(5, 2).raw + 8), 2.5_fx);
    CHECK(abs(a - b) < 0.01_fx);
}


TEST_CASE("граница с float: from_double, from_doubles, to_doubles, quantize_direction") {
    CHECK(Fixed::from_double(1.5) == 1.5_fx);
    CHECK(Fixed::from_double(-0.25) == -0.25_fx);
    CHECK(Fixed::from_double(1e12) == Fixed::from_double(32767.99)); // насыщение
    CHECK(Fixed::from_double(std::nan("")) == Fixed{});
    const WorldPos p = WorldPos::from_doubles(1.5, -2.25, 100.0);
    CHECK(p.x == 98304);
    const auto back = p.to_doubles();
    CHECK(back[0] == doctest::Approx(1.5));
    CHECK(back[1] == doctest::Approx(-2.25));
    const FVec3 d = quantize_direction(3.0, 0.0, 4.0);
    CHECK(d.x.to_double() == doctest::Approx(0.6).epsilon(0.001));
    CHECK(d.z.to_double() == doctest::Approx(0.8).epsilon(0.001));
    CHECK(quantize_direction(0, 0, 0) == FVec3{});
}

TEST_CASE("FVec3: векторное произведение") {
    const FVec3 c = cross({1_fx, 0_fx, 0_fx}, {0_fx, 1_fx, 0_fx});
    CHECK(c == FVec3{0_fx, 0_fx, 1_fx});
    CHECK(2_fx * FVec3{1_fx, 2_fx, 3_fx} == FVec3{2_fx, 4_fx, 6_fx});
}

TEST_CASE("Sdf: плоскость и шар, градиент по умолчанию, луч") {
    const PlaneSdf plane(WorldPos::from_meters(0, 10, 0).y);
    CHECK(plane.sample(WorldPos::from_meters(5, 12, 5)) == 2_fx);
    CHECK(plane.sample(WorldPos::from_meters(5, 7, 5)) == -3_fx);
    const auto hit = raycast(plane, WorldPos::from_meters(0, 50, 0), {0_fx, -1_fx, 0_fx}, 100_fx);
    REQUIRE(hit.has_value());
    CHECK(std::abs(hit->position.y - WorldPos::from_meters(0, 10, 0).y) < 2000);
    CHECK_FALSE(raycast(plane, WorldPos::from_meters(0, 50, 0), {0_fx, 1_fx, 0_fx}, 100_fx).has_value());

    const SphereSdf planet(WorldPos::from_meters(0, 0, 0), 100_fx);
    CHECK(planet.sample(WorldPos::from_meters(0, 150, 0)) == 50_fx);
    CHECK(planet.sample(WorldPos::from_meters(0, 90, 0)) == -10_fx);
    const FVec3 up = planet.gradient(WorldPos::from_meters(0, 101, 0)); // по умолчанию из sample
    CHECK(up.y.to_double() == doctest::Approx(1.0).epsilon(0.01));
    const FVec3 side = planet.gradient(WorldPos::from_meters(101, 0, 0));
    CHECK(side.x.to_double() == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("CRC-32C: контрольный вектор, по частям, чувствительность к одному биту") {
    const auto bytes = [](const char* text) { return std::as_bytes(std::span<const char>(text, std::strlen(text))); };
    CHECK(crc32c(bytes("123456789")) == 0xE3069283u); // общепринятый контрольный вектор CRC-32C
    CHECK(crc32c({}) == 0u);
    const auto part_a = bytes("1234"), part_b = bytes("56789");
    CHECK(crc32c(part_b, crc32c(part_a)) == 0xE3069283u); // считается по частям
    std::vector<std::byte> data(1000);
    Rng rng(3);
    for (std::byte& b : data) b = static_cast<std::byte>(rng.next_u32());
    const std::uint32_t original = crc32c(data);
    for (std::size_t i = 0; i < data.size(); i += 97) {
        data[i] ^= std::byte{0x10};
        CHECK(crc32c(data) != original); // один испорченный бит всегда виден
        data[i] ^= std::byte{0x10};
    }
    CHECK(crc32c(data) == original);
}
