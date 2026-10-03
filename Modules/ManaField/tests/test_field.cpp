#include <ManaField/ManaField.hpp>

#include <doctest/doctest.h>

using namespace ManaField;
using Math::Fixed;
using Math::Mana;
using Math::WorldPos;

namespace {
WorldPos centre_of(std::int64_t x, std::int64_t y, std::int64_t z) { return {x * cell_raw + cell_raw / 2, y * cell_raw + cell_raw / 2, z * cell_raw + cell_raw / 2}; }
const WorldPos middle = centre_of(32, 16, 32);
Config no_relax() {
    Config c;
    c.relax = {};
    return c;
}
} // namespace

TEST_CASE("новое поле пусто: база везде, ни одного чанка") {
    ManaGrid f;
    CHECK(f.allocated_chunks() == 0);
    CHECK(f.density(middle) == f.config().base);
    CHECK(f.density({-5, -5, -5}) == f.config().base);
    f.step(); // шаг пустого поля ничего не создаёт
    CHECK(f.allocated_chunks() == 0);
    CHECK(f.excess_raw() == 0);
}

TEST_CASE("шаг диффузии без источников не меняет сумму маны, в том числе через границы чанков") {
    ManaGrid f(no_relax());
    f.inject(centre_of(15, 5, 5), Mana::from_int(5000));  // последняя ячейка чанка по x
    f.inject(centre_of(-1, -1, -1), Mana::from_int(777)); // угол трёх чанков с отрицательными ключами
    const std::int64_t before = f.excess_raw();
    for (int i = 0; i < 200; ++i) {
        f.step();
        REQUIRE(f.excess_raw() == before);
    }
    CHECK(f.at(15, 5, 5) < f.config().base + Mana::from_int(5000)); // растеклось
    CHECK(f.at(16, 5, 5) > f.config().base);                         // и перешло в соседний чанк
    CHECK(f.allocated_chunks() >= 2);
}

TEST_CASE("диффузия симметрична через границу чанков") {
    ManaGrid f(no_relax());
    f.inject(centre_of(15, 3, 3), Mana::from_int(4000));
    f.inject(centre_of(16, 3, 3), Mana::from_int(4000)); // зеркальная пара относительно плоскости между чанками
    for (int i = 0; i < 100; ++i) f.step();
    for (int k = 0; k < 8; ++k) {
        REQUIRE(f.at(15 - k, 3, 3) == f.at(16 + k, 3, 3));
        REQUIRE(f.at(15, 3 + k, 3) == f.at(16, 3 + k, 3));
    }
}

TEST_CASE("диффузия не создаёт отрицательных значений") {
    ManaGrid f;
    CHECK(f.draw(middle, Fixed::from_int(30), Mana::from_int(1'000'000)).raw() > 0); // выпили всё вокруг
    for (int i = 0; i < 100; ++i) f.step();
    f.for_each_chunk([](const ChunkKey&, std::span<const Fixed> cells) {
        for (const Fixed c : cells) REQUIRE(c.raw >= 0);
    });
}

TEST_CASE("одинаковые поле и вызовы дают одинаковый хеш") {
    const auto run = [] {
        ManaGrid f;
        f.draw(middle, Fixed::from_int(5), Mana::from_int(500));
        f.inject(centre_of(10, 5, 10), Mana::from_int(300));
        for (int i = 0; i < 30; ++i) f.step();
        return f.hash();
    };
    CHECK(run() == run());
    ManaGrid other;
    other.inject(middle, Mana::from_int(1));
    CHECK(other.hash() != run());
}

TEST_CASE("draw: забирает не больше запрошенного и доступного") {
    ManaGrid f;
    const Mana got = f.draw(middle, Fixed::from_int(4), Mana::from_int(100));
    CHECK(got == Mana::from_int(100));
    CHECK(f.excess_raw() == -got.raw());
    const Mana all = f.draw(middle, Fixed::from_int(4), Mana::from_int(1'000'000));
    CHECK(all < Mana::from_int(1'000'000));
    CHECK(f.draw(middle, Fixed::from_int(4), Mana::from_int(1)) == Mana{});
    f.for_each_chunk([](const ChunkKey&, std::span<const Fixed> cells) {
        for (const Fixed c : cells) REQUIRE(c.raw >= 0);
    });
}

TEST_CASE("draw: забор пропорционален плотности") {
    Config config = no_relax();
    config.base = Mana::from_int(30);
    ManaGrid f(config);
    f.inject(centre_of(33, 16, 32), Mana::from_int(90)); // сосед вчетверо плотнее: 120 против 30
    const Mana before_a = f.at(32, 16, 32), before_b = f.at(33, 16, 32);
    (void)f.draw(middle, Fixed::from_int(3), Mana::from_int(60));
    const Mana lost_a = before_a - f.at(32, 16, 32), lost_b = before_b - f.at(33, 16, 32);
    CHECK(lost_b > lost_a * Fixed::from_ratio(39, 10));
    CHECK(lost_b < lost_a * Fixed::from_ratio(41, 10));
}

TEST_CASE("draw через границу чанков забирает из обоих чанков") {
    ManaGrid f;
    const WorldPos on_border = {16 * cell_raw, 5 * cell_raw + cell_raw / 2, 5 * cell_raw + cell_raw / 2}; // между ячейками 15 и 16
    const Mana got = f.draw(on_border, Fixed::from_int(3), Mana::from_int(200));
    CHECK(got == Mana::from_int(200));
    CHECK(f.at(15, 5, 5) < f.config().base);
    CHECK(f.at(16, 5, 5) < f.config().base);
    CHECK(f.allocated_chunks() >= 2);
}

TEST_CASE("поле безгранично: отрицательные и очень далёкие координаты") {
    ManaGrid f;
    const WorldPos far = WorldPos::from_meters(-5'000'000'000LL, 300, 7'000'000'000LL);
    CHECK(f.density(far) == f.config().base);
    f.inject(far, Mana::from_int(10));
    CHECK(f.density(far) == f.config().base + Mana::from_int(10));
    CHECK(f.draw(far, Fixed::from_int(3), Mana::from_int(50)) == Mana::from_int(50));
    for (int i = 0; i < 10; ++i) f.step();
    CHECK(f.allocated_chunks() > 0);
    CHECK(f.density(WorldPos::from_meters(100, 100, 100)) == f.config().base); // рядом с началом координат ничего не выделялось
    CHECK(cell_of({-1, -1, -1}) == CellPos{-1, -1, -1});
    CHECK(chunk_of({-1, 0, 16}) == ChunkKey{-1, 0, 1});
    CHECK(floor_div(-17, 16) == -2);
}

TEST_CASE("чанки освобождаются, когда поле вернулось к базе, хеш пустого поля восстанавливается") {
    ManaGrid f;
    (void)f.draw(middle, Fixed::from_int(3), Mana::from_int(240));
    CHECK(f.allocated_chunks() > 0);
    int steps = 0;
    while (f.allocated_chunks() > 0 && steps < 20000) {
        f.step();
        ++steps;
    }
    CHECK(f.allocated_chunks() == 0);
    CHECK(f.excess_raw() == 0);
    CHECK(f.hash() == ManaGrid{}.hash());
    CHECK(steps < 20000);
}

TEST_CASE("гало: поле не растёт без причины, число чанков ограничено окрестностью дыры") {
    ManaGrid f;
    (void)f.draw(middle, Fixed::from_int(3), Mana::from_int(240));
    for (int i = 0; i < 100; ++i) f.step();
    CHECK(f.allocated_chunks() <= 27); // дыра в одном чанке + гало: не больше блока 3×3×3
}

TEST_CASE("inject и density в разных точках") {
    ManaGrid f;
    f.inject(middle, Mana::from_int(10));
    CHECK(f.density(middle) == f.config().base + Mana::from_int(10));
    f.inject(middle, Mana::from_int(-5)); // отрицательное и нулевое игнорируются
    f.inject(middle, Mana{});
    CHECK(f.density(middle) == f.config().base + Mana::from_int(10));
}

TEST_CASE("дыра в тумане затягивается за 5-10 секунд") {
    ManaGrid f;
    const Mana base = f.config().base;
    (void)f.draw(middle, Fixed::from_int(4), Mana::from_int(800));
    const Mana hole = f.density(middle);
    CHECK(hole < base * Fixed::from_ratio(1, 2)); // дыра заметна: меньше половины нормы
    int steps_to_heal = 0;
    while (f.density(middle) < base * Fixed::from_ratio(9, 10) && steps_to_heal < 1000) {
        f.step();
        ++steps_to_heal;
    }
    const double seconds = steps_to_heal / 10.0; // поле шагает 10 раз в секунду
    CHECK(seconds >= 5.0);
    CHECK(seconds <= 10.0);
}
