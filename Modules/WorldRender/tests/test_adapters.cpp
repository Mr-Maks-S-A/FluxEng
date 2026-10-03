#include <WorldRender/Adapters/Terrain.hpp>
#include <WorldRender/WorldRender.hpp>

#include <doctest/doctest.h>

#include <cmath>

using namespace WorldRender;
using Math::Fixed;
using Math::Mana;

TEST_CASE("TerrainSurface: форма сетки берётся у мира, меш совпадает с мешером Terrain") {
    Terrain::SdfWorld world(1);
    const TerrainSurface surface(world);
    const GridShape shape = surface.shape();
    CHECK(shape.chunk_count() == 256);
    CHECK(shape.chunk_meters == doctest::Approx(16.0));

    SurfaceMesh mesh;
    surface.build({3, 1, 3}, mesh);
    const Terrain::ChunkMesh reference = Terrain::mesh_chunk(world, {3, 1, 3});
    REQUIRE(mesh.vertices.size() == reference.vertices.size());
    CHECK(mesh.indices == reference.indices);
    CHECK(mesh.triangle_count() > 100);
    CHECK(std::memcmp(mesh.vertices.data(), reference.vertices.data(), mesh.vertices.size() * sizeof(SurfaceVertex)) == 0);

    surface.build({0, 3, 0}, mesh); // небо: буфер очищается сам
    CHECK(mesh.vertices.empty());
}

TEST_CASE("TerrainSurface: мир другой формы даёт другую сетку, индексы чанков переводятся") {
    Terrain::SdfWorld small(1, Terrain::Layout{2, 1, 3});
    const TerrainSurface surface(small);
    CHECK(surface.shape().chunk_count() == 6);
    const std::vector<Terrain::ChunkCoord> dirty = small.take_dirty();
    const std::vector<ChunkIndex> indices = TerrainSurface::indices(dirty);
    REQUIRE(indices.size() == 6);
    for (const ChunkIndex c : indices) CHECK(surface.shape().contains(c));
    MeshQueue queue(surface.shape());
    queue.push(indices);
    CHECK(queue.size() == 6);
}

TEST_CASE("ManaFogSource: слой у поверхности, яркость = плотность / база, дыра после draw темнее") {
    Terrain::SdfWorld world(1);
    ManaField::ManaGrid field;
    const ManaFogSource fog(field, world);
    CHECK(fog.cell_meters() == doctest::Approx(2.0));
    const CellBox box = fog.bounds();
    CHECK(box.hi[0] == 63);
    CHECK(box.hi[1] == 31);

    const std::int64_t gx = 64 * 65536, gz = 64 * 65536;
    const double ground = static_cast<double>(world.ground_height(gx, gz)) / 65536.0;
    const glm::dvec3 eye{64.0, ground + 4.0, 64.0};
    std::vector<FogVertex> before;
    const std::size_t cells = build_fog_vertices(fog, eye, 20.0f, before);
    REQUIRE(cells > 100);
    float lowest = 1e9f, highest = -1e9f;
    for (const FogVertex& v : before) {
        CHECK(v.density == doctest::Approx(1.0f).epsilon(0.01));
        lowest = std::min(lowest, v.center[1] + static_cast<float>(eye.y));
        highest = std::max(highest, v.center[1] + static_cast<float>(eye.y));
    }
    CHECK(lowest > static_cast<float>(ground) - 8.0f);  // в породе тумана нет
    CHECK(highest < static_cast<float>(ground) + 14.0f); // и высоко над поверхностью тоже

    (void)field.draw({gx, static_cast<std::int64_t>((ground + 4.0) * 65536), gz}, Fixed::from_int(3), Mana::from_int(240));
    std::vector<FogVertex> after;
    build_fog_vertices(fog, eye, 20.0f, after);
    float dimmest = 1.0f;
    for (const FogVertex& v : after) dimmest = std::min(dimmest, v.density);
    CHECK(dimmest < 0.7f); // дыра заметна
}

TEST_CASE("слой тумана настраивается: выше порога ничего, глубже метра в породе ничего") {
    Terrain::SdfWorld world(1);
    ManaField::ManaGrid field;
    const std::int64_t gx = 64 * 65536, gz = 64 * 65536;
    const double ground = static_cast<double>(world.ground_height(gx, gz)) / 65536.0;
    const auto cell_above = [&](double meters_above) { return static_cast<std::int64_t>((ground + meters_above) / 2.0); };
    const ManaFogSource thin(field, world, 3.0), thick(field, world, 30.0);
    const std::int64_t cx = 32, cz = 32;
    CHECK(thick.density(cx, cell_above(20.0), cz) > 0.9f);
    CHECK(thin.density(cx, cell_above(20.0), cz) == 0.0f);
    CHECK(thick.density(cx, cell_above(-12.0), cz) == 0.0f); // глубоко в породе
}

TEST_CASE("TerrainHeights: карта высот совпадает с ground_height и обновляется по чанку") {
    Terrain::SdfWorld world(1);
    WorldRender::TerrainHeights source(world);
    WorldRender::HeightMap map(128, 128, 1.0);
    map.update(source);
    const double h = map.height_at(40.0, 40.0);
    CHECK_FALSE(std::isnan(h));
    CHECK(h == doctest::Approx(source.height_at(40.5, 40.5)).epsilon(0.2));
    // правка в одном чанке: пересчитывается только его прямоугольник
    const WorldRender::CellRegion region = WorldRender::TerrainHeights::cells_of_chunk({2, 1, 2}, map);
    CHECK(region.x0 == 32);
    CHECK(region.x1 == 48);
    CHECK(map.update(source, region) == 16 * 16);
    const RendererSystem::Image picture = map.shade();
    CHECK(picture.width() == 128);
    CHECK(picture.pixel(64, 64).a == 255);
}
