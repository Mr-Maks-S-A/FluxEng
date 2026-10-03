#include <Character/Character.hpp>
#include <Terrain/Terrain.hpp>

#include <doctest/doctest.h>

#include <cmath>

using namespace Character;
using Math::Fixed;
using Math::FVec3;
using Math::WorldPos;

namespace {

struct Rig {
    Terrain::SdfWorld terrain{1};
    ECS::World world;
    ECS::Entity who;

    explicit Rig(double x = 64, double z = 64, double above = 3.0) {
        const std::int64_t gx = static_cast<std::int64_t>(x * 65536), gz = static_cast<std::int64_t>(z * 65536);
        const WorldPos feet{gx, terrain.ground_height(gx, gz) + static_cast<std::int64_t>(above * 65536), gz};
        who = spawn(world, feet, {Math::Mana::from_int(10), Math::Mana::from_int(100), Math::Mana::from_int(5)}, {});
    }
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) step(world, terrain);
    }
    Position& pos() { return *world.get<Position>(who); }
    Motor& motor() { return *world.get<Motor>(who); }
    [[nodiscard]] double y() { return static_cast<double>(pos().value.y) / 65536.0; }
};

} // namespace

TEST_CASE("падает на землю и встаёт на ней, не проваливаясь") {
    Rig r;
    r.run(120);
    CHECK(r.motor().grounded);
    const double ground = static_cast<double>(r.terrain.ground_height(r.pos().value.x, r.pos().value.z)) / 65536.0;
    CHECK(std::abs(r.y() - ground) < 0.35);
    const WorldPos at = r.pos().value;
    r.run(120); // стоит неподвижно: без дрожи и сползания
    CHECK(r.pos().value == at);
}

TEST_CASE("прыжок поднимает примерно на 2,9 м и возвращает на землю") {
    Rig r;
    r.run(120);
    const double ground = r.y();
    r.motor().jump = true;
    double apex = ground;
    for (int i = 0; i < 120; ++i) {
        step(r.world, r.terrain);
        apex = std::max(apex, r.y());
    }
    CHECK(apex - ground > 2.5);
    CHECK(apex - ground < 3.2);
    CHECK(r.motor().grounded);
}

TEST_CASE("ходьба через весь мир: не проваливается и не застревает на границах чанков") {
    Rig r(4, 64, 2);
    r.run(60);
    r.motor().wish = {Fixed::from_int(1), Fixed{}, Fixed{}};
    double max_depth_error = 0;
    for (int i = 0; i < 60 * 30; ++i) { // 30 секунд при 5 м/с = 150 м > ширины мира
        step(r.world, r.terrain);
        const double ground = static_cast<double>(r.terrain.ground_height(r.pos().value.x, r.pos().value.z)) / 65536.0;
        max_depth_error = std::max(max_depth_error, ground - r.y());
    }
    CHECK(max_depth_error < 0.6); // не уходит под поверхность
    const double x = static_cast<double>(r.pos().value.x) / 65536.0;
    CHECK(x > 120.0); // дошёл до дальнего края
    CHECK(x < 128.5); // и упёрся в стену мира
}

TEST_CASE("упирается в стены мира по всем краям") {
    Rig r(64, 64, 2);
    r.run(60);
    for (const FVec3 dir : {FVec3{Fixed::from_int(-1), {}, {}}, FVec3{{}, {}, Fixed::from_int(1)}, FVec3{{}, {}, Fixed::from_int(-1)}}) {
        r.motor().wish = dir;
        r.run(60 * 25);
        const WorldPos p = r.pos().value;
        CHECK(p.x >= -655360);
        CHECK(p.x <= r.terrain.layout().size_x() + 655360);
        CHECK(p.z >= -655360);
        CHECK(p.z <= r.terrain.layout().size_z() + 655360);
        CHECK(p.y > 0);
    }
}

TEST_CASE("вырезать землю под ногами: падает в яму и выходит из неё") {
    // Игрок выбирает сторону, где выбраться проще: хотя бы в одну из четырёх выход должен найтись.
    int escaped = 0;
    for (const FVec3 dir : {FVec3{Fixed::from_int(1), {}, {}}, FVec3{Fixed::from_int(-1), {}, {}}, FVec3{{}, {}, Fixed::from_int(1)}, FVec3{{}, {}, Fixed::from_int(-1)}}) {
        Rig r(64, 64, 0.2);
        r.run(90);
        REQUIRE(r.motor().grounded);
        const double ground = r.y();
        const WorldPos start = r.pos().value;
        r.terrain.carve_sphere(start, Fixed::from_int(2)); // шар радиуса 2 м с центром у ног
        r.run(60);
        CHECK(r.y() < ground - 1.0); // упал на дно ямы
        CHECK(r.motor().grounded);

        r.motor().wish = dir;
        bool out = false;
        for (int i = 0; i < 60 * 8 && !out; ++i) {
            if (i % 20 == 0) r.motor().jump = true;
            step(r.world, r.terrain);
            const WorldPos p = r.pos().value;
            const double gh = static_cast<double>(r.terrain.ground_height(p.x, p.z)) / 65536.0;
            const double away = std::hypot(static_cast<double>(p.x - start.x), static_cast<double>(p.z - start.z)) / 65536.0;
            out = r.y() > gh - 0.3 && away > 2.5;
        }
        escaped += out;
    }
    CHECK(escaped >= 2);
}

TEST_CASE("личная мана восстанавливается с постоянной скоростью до максимума") {
    Rig r;
    r.run(60);
    CHECK(r.world.get<ManaPool>(r.who)->current.to_double() == doctest::Approx(15.0).epsilon(0.01)); // 10 + 5/с · 1 с
    r.run(60 * 60);
    CHECK(r.world.get<ManaPool>(r.who)->current == Math::Mana::from_int(100));
}

TEST_CASE("детерминизм: два прогона одного сценария дают один хеш") {
    const auto run = [] {
        Rig r(30, 30, 2);
        r.motor().wish = {Fixed::from_ratio(7, 10), Fixed{}, Fixed::from_ratio(7, 10)};
        for (int i = 0; i < 600; ++i) {
            if (i % 70 == 0) r.motor().jump = true;
            step(r.world, r.terrain);
        }
        Math::Hasher h;
        hash_characters(r.world, h);
        return h.value();
    };
    CHECK(run() == run());
}

// ---- Движение по аналитическим полям: без ландшафта, мгновенно — Character не знает, по чему идёт ----

namespace {
struct Spawn {
    ECS::World world;
    ECS::Entity who;
    explicit Spawn(WorldPos feet) { who = spawn(world, feet, {Math::Mana::from_int(10), Math::Mana::from_int(100), Math::Mana::from_int(5)}, {}); }
    Position& pos() { return *world.get<Position>(who); }
    Motor& motor() { return *world.get<Motor>(who); }
};
} // namespace

TEST_CASE("плоскость: падает, встаёт и ходит без дрожи") {
    const Math::PlaneSdf plane(WorldPos::from_meters(0, 10, 0).y);
    Spawn s(WorldPos::from_meters(5, 14, 5));
    for (int i = 0; i < 120; ++i) step(s.world, plane);
    CHECK(s.motor().grounded);
    CHECK(std::abs(s.pos().value.y - plane.height) < 3000); // стоит на плоскости (±5 см)
    const WorldPos rest = s.pos().value;
    for (int i = 0; i < 60; ++i) step(s.world, plane);
    CHECK(s.pos().value == rest);
    s.motor().wish = {Fixed::from_int(1), Fixed{}, Fixed{}};
    for (int i = 0; i < 60; ++i) step(s.world, plane);
    CHECK(s.pos().value.x - rest.x > WorldPos::from_meters(4, 0, 0).x); // ≈ 5 м за секунду
    CHECK(std::abs(s.pos().value.y - plane.height) < 3000);
}

TEST_CASE("шар-планета: персонаж встаёт на вершину") {
    const Math::SphereSdf planet(WorldPos::from_meters(0, 0, 0), Fixed::from_int(100));
    Spawn s(WorldPos::from_meters(0, 104, 0));
    for (int i = 0; i < 120; ++i) step(s.world, planet);
    CHECK(s.motor().grounded);
    CHECK(std::abs(s.pos().value.y - WorldPos::from_meters(0, 100, 0).y) < 3000);
}
