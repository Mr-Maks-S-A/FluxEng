#include <Terrain/Mesher.hpp>
#include <Terrain/Terrain.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <cmath>
#include <set>

using namespace Terrain;
using Math::Fixed;
using Math::FVec3;
using Math::WorldPos;

namespace {
WorldPos meters(double x, double y, double z) {
    return {static_cast<std::int64_t>(x * 65536), static_cast<std::int64_t>(y * 65536), static_cast<std::int64_t>(z * 65536)};
}
const FVec3 down{Fixed{}, Fixed::from_int(-1), Fixed{}};
} // namespace

TEST_CASE("мир: размеры и объём данных") {
    constexpr Layout layout{};
    CHECK(layout.chunk_count() == 256);
    CHECK(sizeof(Chunk) * static_cast<std::size_t>(layout.chunk_count()) < 26'000'000);
    CHECK(layout.size_x() == 255 * 32768);
    CHECK(layout.coord(layout.index({3, 2, 5})) == ChunkCoord{3, 2, 5});
}

TEST_CASE("генерация: один сид один мир, разные сиды разные") {
    SdfWorld a(7), b(7), c(8);
    CHECK(a.hash() == b.hash());
    CHECK(a.hash() != c.hash());
}

TEST_CASE("порода ниже поверхности, воздух выше") {
    SdfWorld w(1);
    const std::int64_t h = w.ground_height(meters(64, 0, 64).x, meters(64, 0, 64).z);
    CHECK(h > meters(0, 8, 0).y);
    CHECK(h < meters(0, 45, 0).y);
    CHECK(w.sample({meters(64, 0, 64).x, h - meters(0, 2, 0).y, meters(64, 0, 64).z}).raw < 0);
    CHECK(w.sample({meters(64, 0, 64).x, h + meters(0, 2, 0).y, meters(64, 0, 64).z}).raw > 0);
}

TEST_CASE("после вырезания сферы sample в её центре положителен") {
    SdfWorld w(1);
    const WorldPos c = meters(64, 0, 64);
    const WorldPos centre{c.x, w.ground_height(c.x, c.z) - meters(0, 3, 0).y, c.z};
    CHECK(w.sample(centre).raw < 0);
    const EditResult r = w.carve_sphere(centre, Fixed::from_int(3));
    CHECK(r.changed());
    CHECK(w.sample(centre).raw > 0);
    // Насыпанная сфера, наоборот, твёрдая.
    const WorldPos sky = meters(40, 55, 40);
    CHECK(w.sample(sky).raw > 0);
    w.add_sphere(sky, Fixed::from_int(3));
    CHECK(w.sample(sky).raw < 0);
}

TEST_CASE("правка возвращает затронутые чанки с границами") {
    SdfWorld w(1);
    (void)w.take_dirty();
    const EditResult r = w.carve_sphere(meters(64, 16, 64), Fixed::from_int(2)); // центр: углы 8 чанков
    CHECK(r.chunks.size() >= 8);
    CHECK_FALSE(r.bounds.empty());
    for (const int a : {0, 1, 2}) CHECK((r.bounds.lo[a] <= 32 * (a == 1 ? 1 : 4) && r.bounds.hi[a] >= 32 * (a == 1 ? 1 : 4)));
    CHECK(w.take_dirty().size() == r.chunks.size());
    CHECK(w.take_dirty().empty());
    CHECK_FALSE(w.carve_sphere(meters(500, 20, 64), Fixed::from_int(2)).changed()); // вне мира
}

TEST_CASE("один сид и один список правок дают один хеш") {
    const auto build = [] {
        SdfWorld w(5);
        w.carve_sphere(meters(30, 15, 30), Fixed::from_int(4));
        w.add_sphere(meters(80, 40, 50), Fixed::from_int(5));
        w.carve_sphere(meters(32, 15, 60), Fixed::from_ratio(5, 2));
        return w.hash();
    };
    CHECK(build() == build());
    SdfWorld plain(5);
    CHECK(plain.hash() != build());
}

TEST_CASE("raycast: вниз попадает в поверхность, вверх уходит в небо") {
    SdfWorld w(1);
    const WorldPos from = meters(60, 62, 60);
    const auto hit = w.raycast(from, down, Fixed::from_int(100));
    REQUIRE(hit.has_value());
    CHECK(std::abs(hit->position.y - w.ground_height(from.x, from.z)) < meters(0, 0.4, 0).y);
    CHECK_FALSE(w.raycast(from, {Fixed{}, Fixed::from_int(1), Fixed{}}, Fixed::from_int(100)).has_value());
    CHECK_FALSE(w.raycast(from, down, Fixed::from_int(2)).has_value()); // не дотянулся
}

TEST_CASE("gradient в воздухе над землёй смотрит вверх") {
    SdfWorld w(1);
    const WorldPos p{meters(60, 0, 60).x, w.ground_height(meters(60, 0, 60).x, meters(60, 0, 60).z) + meters(0, 1, 0).y, meters(60, 0, 60).z};
    CHECK(w.gradient(p).y > Fixed::from_ratio(1, 2));
}

TEST_CASE("мир ограничен: за краем порода, сверху воздух") {
    SdfWorld w(1);
    CHECK(w.sample(meters(-1, 30, 50)).raw < 0);
    CHECK(w.sample(meters(130, 30, 50)).raw < 0);
    CHECK(w.sample(meters(50, -3, 50)).raw < 0);
    CHECK(w.sample(meters(50, 70, 50)).raw > 0);
}

TEST_CASE("сетка: есть поверхность, нормали единичные, обход наружу") {
    SdfWorld w(1);
    const ChunkMesh m = mesh_chunk(w, {3, 1, 3});
    CHECK(m.triangle_count() > 100);
    for (const Vertex& v : m.vertices) {
        const float len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
        REQUIRE(std::abs(len - 1.0f) < 1e-3f);
        for (int i = 0; i < 3; ++i) REQUIRE((v.position[i] >= -1e-4f && v.position[i] <= 16.0001f));
    }
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const Vertex &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]], &c = m.vertices[m.indices[i + 2]];
        const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        REQUIRE(cr[0] * a.normal[0] + cr[1] * a.normal[1] + cr[2] * a.normal[2] >= -1e-9f);
    }
}

TEST_CASE("сетка: пустой и сплошной чанки пусты") {
    SdfWorld w(1);
    CHECK(mesh_chunk(w, {0, 3, 0}).vertices.empty()); // небо
    CHECK(mesh_chunk(w, {0, 0, 0}).vertices.empty()); // толща породы
}

namespace {
/// Вершины чанка на грани оси `axis` (local = 0 или 16 м), в мировых метрах, отсортированные.
std::vector<std::array<float, 3>> face_vertices(const SdfWorld& w, ChunkCoord c, int axis, float local) {
    const ChunkMesh m = mesh_chunk(w, c);
    std::vector<std::array<float, 3>> out;
    const int origin[3] = {c.x, c.y, c.z};
    for (const Vertex& v : m.vertices) {
        if (v.position[axis] != local) continue;
        out.push_back({v.position[0] + static_cast<float>(chunk_origin_meters(origin[0])), v.position[1] + static_cast<float>(chunk_origin_meters(origin[1])),
                       v.position[2] + static_cast<float>(chunk_origin_meters(origin[2]))});
    }
    std::ranges::sort(out);
    return out;
}
} // namespace

TEST_CASE("швов нет: вершины на общей грани соседних чанков совпадают") {
    SdfWorld w(3);
    w.carve_sphere(meters(32, 22, 40), Fixed::from_int(5)); // яма поперёк границ чанков
    int compared = 0;
    for (int axis = 0; axis < 3; ++axis) {
        for (int cx = 0; cx < w.layout().chunks_x - 1; ++cx) {
            for (int cy = 0; cy < w.layout().chunks_y - 1; ++cy) {
                for (int cz = 0; cz < w.layout().chunks_z - 1; ++cz) {
                    const ChunkCoord a{cx, cy, cz};
                    ChunkCoord b = a;
                    (axis == 0 ? b.x : axis == 1 ? b.y : b.z) += 1;
                    const auto va = face_vertices(w, a, axis, 16.0f), vb = face_vertices(w, b, axis, 0.0f);
                    REQUIRE(va.size() == vb.size());
                    for (std::size_t i = 0; i < va.size(); ++i)
                        for (int k = 0; k < 3; ++k) REQUIRE(std::abs(va[i][static_cast<std::size_t>(k)] - vb[i][static_cast<std::size_t>(k)]) < 1e-4f);
                    compared += static_cast<int>(va.size());
                }
            }
        }
    }
    CHECK(compared > 50); // граничные вершины действительно были
}

TEST_CASE("правка обновляет сетки соседей, читающих край") {
    SdfWorld w(3);
    (void)w.take_dirty();
    // Правка у самой границы чанков x: 15|16 (отсчёты 31|32) должна пометить оба чанка.
    const EditResult r = w.carve_sphere(meters(16, 16, 16), Fixed::from_ratio(1, 2));
    std::set<int> ids;
    for (const ChunkCoord c : r.chunks) ids.insert(w.layout().index(c));
    CHECK(ids.contains(w.layout().index({0, 0, 0})));
    CHECK(ids.contains(w.layout().index({1, 0, 0})));
}

TEST_CASE("mesh_chunk в готовый буфер даёт ту же сетку и переиспользует память") {
    SdfWorld w(1);
    ChunkMesh reused;
    mesh_chunk(w, {3, 1, 3}, reused);
    const ChunkMesh fresh = mesh_chunk(w, {3, 1, 3});
    REQUIRE(reused.vertices.size() == fresh.vertices.size());
    CHECK(reused.indices == fresh.indices);
    const auto* data = reused.vertices.data();
    mesh_chunk(w, {3, 1, 3}, reused);
    CHECK(reused.vertices.data() == data); // тот же блок памяти
    mesh_chunk(w, {0, 3, 0}, reused);      // небо: буфер очищается
    CHECK(reused.vertices.empty());
}

TEST_CASE("SdfWorld — это Math::SdfField: градиент и луч через интерфейс, ground_height по WorldPos") {
    SdfWorld w(1);
    const Math::SdfField& field = w;
    const WorldPos column = meters(60, 0, 60);
    const WorldPos above{column.x, w.ground_height(column) + meters(0, 1, 0).y, column.z};
    CHECK(field.gradient(above).y > Fixed::from_ratio(1, 2));
    CHECK(Math::raycast(field, meters(60, 62, 60), down, Fixed::from_int(100)).has_value());
}

namespace {
/// Плоский мир: порода ниже y = 10 м. Источник чанков, не связанный с шумом.
struct FlatSource final : ChunkSource {
    void generate(ChunkCoord c, Chunk& chunk) const override {
        for (int y = 0; y < chunk_size; ++y)
            for (int z = 0; z < chunk_size; ++z)
                for (int x = 0; x < chunk_size; ++x) {
                    const std::int64_t wy = static_cast<std::int64_t>(c.y * chunk_size + y) * cell_raw;
                    const std::int64_t d = std::clamp<std::int64_t>((wy - 10 * 65536) / 128, -32767, 32767);
                    chunk.distance[static_cast<std::size_t>(local_index(x, y, z))] = static_cast<std::int16_t>(d);
                    chunk.material[static_cast<std::size_t>(local_index(x, y, z))] = d < 0 ? Rock : Air;
                }
    }
};
} // namespace

TEST_CASE("форма мира — данные: маленький мир, свой источник чанков, два мира в одном процессе") {
    const Layout small{2, 1, 3};
    SdfWorld flat(small, std::make_unique<FlatSource>());
    SdfWorld hills(1); // стандартный 8×4×8 рядом
    CHECK(flat.layout() == small);
    CHECK(hills.layout() == Layout{});
    CHECK(flat.dirty_count() == 6);
    CHECK(hills.dirty_count() == 256);

    CHECK(flat.sample(meters(5, 14, 5)) == Fixed::from_int(4));  // над плоскостью
    CHECK(flat.sample(meters(5, 6, 5)) == Fixed::from_int(-4));  // под ней
    CHECK(flat.sample(meters(5, 14, 5)).raw != hills.sample(meters(5, 14, 5)).raw);
    // Стены мира по его размеру: 2 чанка = 32 м по x, 3 чанка = 48 м по z.
    CHECK(flat.sample(meters(31, 14, 5)).raw > 0);
    CHECK(flat.sample(meters(33, 14, 5)).raw < 0);
    CHECK(flat.sample(meters(5, 14, 47)).raw > 0);
    CHECK(flat.sample(meters(5, 14, 49)).raw < 0);

    const EditResult r = flat.carve_sphere(meters(16, 10, 24), Fixed::from_int(3));
    CHECK(r.changed());
    for (const ChunkCoord c : r.chunks) CHECK(small.contains(c));
    CHECK(flat.sample(meters(16, 10, 24)).raw > 0);
    CHECK(mesh_chunk(flat, {0, 0, 1}).triangle_count() > 0); // сетка строится по любой форме
    CHECK(flat.hash() != SdfWorld(small, std::make_unique<FlatSource>()).hash());
}

TEST_CASE("distance_at в метрах согласован с sample в отсчёте") {
    SdfWorld w(1);
    const int sx = 100, sy = 40, sz = 90;
    const WorldPos at{sx * cell_raw, sy * cell_raw, sz * cell_raw};
    CHECK(w.distance_at(sx, sy, sz) == w.sample(at));
    CHECK(w.raw_distance_at(sx, sy, sz) * 128 == w.distance_at(sx, sy, sz).raw);
}

TEST_CASE("load_chunk: снимок изменённых чанков + сид дают тот же мир, что и правки") {
    SdfWorld edited(5);
    (void)edited.carve_sphere(meters(30, 20, 30), Fixed::from_int(4));
    (void)edited.add_sphere(meters(100, 45, 70), Fixed::from_int(5));
    SdfWorld restored(5);
    (void)restored.take_dirty();
    int copied = 0;
    for (int i = 0; i < edited.layout().chunk_count(); ++i) {
        const ChunkCoord c = edited.layout().coord(i);
        if (!edited.modified(c)) continue; // нетронутые чанки восстанавливает сид
        restored.load_chunk(c, edited.chunk(c));
        ++copied;
    }
    CHECK(copied > 0);
    CHECK(copied < 40); // изменено мало: снимок маленький
    CHECK(restored.hash() == edited.hash());
    CHECK(restored.dirty_count() >= static_cast<std::size_t>(copied)); // сетки перестроятся: чанки и соседи в очереди
    CHECK_FALSE(SdfWorld(5).modified({3, 1, 3}));
    CHECK(edited.modified(ChunkCoord{0, 0, 0}) == edited.modified({0, 0, 0}));
}
