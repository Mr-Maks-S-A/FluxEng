#include <WorldRender/WorldRender.hpp>

#include <doctest/doctest.h>

#include <cmath>

using namespace WorldRender;
using Math::Fixed;

namespace {

/// Источник тумана-заглушка: яркость задаёт функция; рисуется только коробка `box`.
struct FakeFog final : FogSource {
    CellBox box{{0, 0, 0}, {9, 9, 9}};
    float (*fn)(std::int64_t, std::int64_t, std::int64_t) = [](std::int64_t, std::int64_t, std::int64_t) { return 1.0f; };
    double cell_meters() const override { return 2.0; }
    CellBox bounds() const override { return box; }
    float density(std::int64_t x, std::int64_t y, std::int64_t z) const override { return fn(x, y, z); }
};

} // namespace

TEST_CASE("GridShape: номера и углы чанков") {
    const GridShape shape{.chunks_x = 4, .chunks_y = 2, .chunks_z = 3, .chunk_meters = 16.0, .origin = {100.0, 0.0, -50.0}};
    CHECK(shape.chunk_count() == 24);
    CHECK(shape.coord(shape.index({3, 1, 2})) == ChunkIndex{3, 1, 2});
    CHECK(shape.contains({3, 1, 2}));
    CHECK_FALSE(shape.contains({4, 0, 0}));
    CHECK_FALSE(shape.contains({0, -1, 0}));
    const glm::dvec3 corner = shape.chunk_origin({2, 1, 0});
    CHECK(corner.x == doctest::Approx(132.0));
    CHECK(corner.y == doctest::Approx(16.0));
    CHECK(corner.z == doctest::Approx(-50.0));
    CHECK(sizeof(SurfaceVertex) == 28);
}

TEST_CASE("MeshQueue: без повторов, ближние чанки первыми") {
    MeshQueue q{GridShape{.chunks_x = 8, .chunks_y = 4, .chunks_z = 8, .chunk_meters = 16.0}};
    q.push({7, 3, 7});
    q.push({0, 0, 0});
    q.push({7, 3, 7}); // повтор
    q.push({3, 1, 3});
    CHECK(q.size() == 3);
    CHECK(q.contains({0, 0, 0}));
    const auto first = q.pop_nearest(1, glm::dvec3{2.0, 2.0, 2.0});
    REQUIRE(first.size() == 1);
    CHECK(first[0] == ChunkIndex{0, 0, 0});
    CHECK_FALSE(q.contains({0, 0, 0}));
    const auto rest = q.pop_nearest(100, glm::dvec3{120.0, 60.0, 120.0});
    REQUIRE(rest.size() == 2);
    CHECK(rest[0] == ChunkIndex{7, 3, 7});
    CHECK(q.size() == 0);
    q.push({0, 0, 0}); // можно поставить снова после выдачи
    CHECK(q.size() == 1);
}

TEST_CASE("MeshQueue работает с любой формой сетки, в том числе смещённой от начала координат") {
    MeshQueue q{GridShape{.chunks_x = 2, .chunks_y = 1, .chunks_z = 2, .chunk_meters = 100.0, .origin = {1000.0, 0.0, 0.0}}};
    q.push({0, 0, 0});
    q.push({1, 0, 1});
    const auto nearest = q.pop_nearest(1, glm::dvec3{1190.0, 50.0, 190.0}); // у дальнего угла сетки
    REQUIRE(nearest.size() == 1);
    CHECK(nearest[0] == ChunkIndex{1, 0, 1});
}

TEST_CASE("DebugDraw: рамка — 12 рёбер, крест — 3 линии, очистка") {
    DebugDraw d;
    d.box({0, 0, 0}, {1, 2, 3}, RendererSystem::Colors::white);
    CHECK(d.line_count() == 12);
    d.cross({5, 5, 5}, 1.0, RendererSystem::Colors::red);
    CHECK(d.line_count() == 15);
    d.clear();
    CHECK(d.empty());
}

TEST_CASE("рамки чанков: по коробке на чанк любой сетки") {
    DebugDraw d;
    TerrainView::add_chunk_boxes(d, GridShape{.chunks_x = 3, .chunks_y = 2, .chunks_z = 2}, RendererSystem::Colors::white);
    CHECK(d.line_count() == 12 * 12);
}

TEST_CASE("look_direction: единичный вектор, yaw 0 вдоль +X") {
    const glm::vec3 f = look_direction(0.0f, 0.0f);
    CHECK(f.x == doctest::Approx(1.0f));
    const glm::vec3 g = look_direction(1.2f, 0.7f);
    CHECK(glm::length(g) == doctest::Approx(1.0f));
    CHECK(look_direction(0.0f, 0.5f).y > 0.0f);
}

TEST_CASE("камера: относительный вид, глаз в нуле; следящая не заходит в поверхность (любую Math::SdfField)") {
    const Math::PlaneSdf ground(Math::WorldPos::from_meters(0, 10, 0).y); // плоский мир: земля на 10 м
    CameraRig rig;
    rig.yaw = 0.3f;
    rig.pitch = -0.2f;
    const glm::dvec3 target{64.0, 11.6, 64.0};
    const View v = rig.view(target, ground, {1280.0f, 720.0f});
    CHECK(v.camera.position == glm::vec3(0.0f));
    CHECK(glm::length(v.relative(v.eye)) == doctest::Approx(0.0f));
    CHECK(glm::length(glm::vec3(target - v.eye)) <= rig.distance + 0.01f);
    CHECK(glm::dot(v.right, rig.forward()) == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(v.eye.y > 10.0);

    // Взгляд вниз: камера оказалась бы под землёй — её прижимает ближе к цели.
    rig.pitch = 1.2f;
    const View low = rig.view(target, ground, {1280.0f, 720.0f});
    CHECK(low.eye.y > 10.0);
    CHECK(glm::length(glm::vec3(target - low.eye)) < rig.distance);

    // Тот же код — на шаре-планете.
    const Math::SphereSdf planet(Math::WorldPos::from_meters(0, -100, 0), Fixed::from_int(110));
    const View on_planet = rig.view({0.0, 11.6, 0.0}, planet, {1280.0f, 720.0f});
    CHECK(planet.sample(Math::WorldPos::from_doubles(on_planet.eye.x, on_planet.eye.y, on_planet.eye.z)).raw > 0);
}

TEST_CASE("свободная камера летит по взгляду, переключение сохраняет место") {
    const Math::PlaneSdf ground(0);
    CameraRig rig;
    (void)rig.view({60.0, 40.0, 60.0}, ground, {100.0f, 100.0f});
    rig.toggle();
    REQUIRE(rig.is_free());
    const glm::dvec3 before = rig.position();
    rig.yaw = 0.0f;
    rig.pitch = 0.0f;
    rig.fly(1.0f, 0.0f, 0.0f, 0.5f);
    CHECK(rig.position().x == doctest::Approx(before.x + 0.5 * rig.free_speed));
    const View v = rig.view({0.0, 0.0, 0.0}, ground, {100.0f, 100.0f}); // цель игнорируется
    CHECK(v.eye == rig.position());
}

TEST_CASE("туман: ячейки в радиусе, четыре вершины на ячейку, яркость берётся у источника") {
    FakeFog fog;
    std::vector<FogVertex> vertices;
    const std::size_t cells = build_fog_vertices(fog, glm::dvec3{10.0, 10.0, 10.0}, 6.0f, vertices);
    REQUIRE(cells > 10);
    CHECK(vertices.size() == cells * 4);
    for (const FogVertex& v : vertices) {
        CHECK(v.density == doctest::Approx(1.0f));
        const double d2 = static_cast<double>(v.center[0]) * v.center[0] + static_cast<double>(v.center[1]) * v.center[1] + static_cast<double>(v.center[2]) * v.center[2];
        CHECK(d2 <= 36.0 + 1e-3); // центры в радиусе; позиции — от глаза
    }
}

TEST_CASE("туман: ячейки с нулевой яркостью скрыты, границы источника уважаются") {
    FakeFog hidden;
    hidden.fn = [](std::int64_t, std::int64_t y, std::int64_t) { return y < 3 ? 1.0f : 0.0f; }; // «слой» только у земли
    std::vector<FogVertex> vertices;
    build_fog_vertices(hidden, glm::dvec3{10.0, 4.0, 10.0}, 20.0f, vertices);
    REQUIRE_FALSE(vertices.empty());
    for (const FogVertex& v : vertices) CHECK(v.center[1] + 4.0f < 6.0f); // мир-y центра ячейки < 6 м (y-индекс < 3)

    FakeFog small;
    small.box = {{2, 2, 2}, {3, 3, 3}}; // всего 8 ячеек
    CHECK(build_fog_vertices(small, glm::dvec3{6.0, 6.0, 6.0}, 100.0f, vertices) == 8);
    FakeFog empty;
    empty.box = {{0, 0, 0}, {-1, -1, -1}};
    CHECK(build_fog_vertices(empty, glm::dvec3{0.0, 0.0, 0.0}, 100.0f, vertices) == 0);
}

TEST_CASE("туман: дыра в источнике видна как провал яркости") {
    FakeFog fog;
    fog.fn = [](std::int64_t x, std::int64_t y, std::int64_t z) { return (x == 5 && y == 5 && z == 5) ? 0.2f : 1.0f; };
    std::vector<FogVertex> vertices;
    build_fog_vertices(fog, glm::dvec3{11.0, 11.0, 11.0}, 30.0f, vertices);
    float dimmest = 1.0f;
    for (const FogVertex& v : vertices) dimmest = std::min(dimmest, v.density);
    CHECK(dimmest == doctest::Approx(0.2f));
}
