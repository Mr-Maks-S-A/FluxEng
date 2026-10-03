/**
 * @example 02_custom_world.cpp
 * Форма мира и источник чанков — данные: маленький плоский мир рядом со стандартным, свой `ChunkSource`.
 *
 * Так делаются измерения, планеты и тесты: `SdfWorld(layout, source)` — любой размер и любая генерация.
 */

#include <Terrain/Mesher.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>

using namespace Terrain;
using Math::Fixed;
using Math::WorldPos;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/// Свой источник: плоский мир, порода ниже 10 м. Источник заполняет чанк отсчётами (расстояние Q8.8 в ячейках и материал).
struct FlatSource final : ChunkSource {
    void generate(ChunkCoord coord, Chunk& chunk) const override {
        for (int y = 0; y < chunk_size; ++y)
            for (int z = 0; z < chunk_size; ++z)
                for (int x = 0; x < chunk_size; ++x) {
                    const std::int64_t world_y = static_cast<std::int64_t>(coord.y * chunk_size + y) * cell_raw; // WorldPos
                    const std::int64_t d = std::clamp<std::int64_t>((world_y - 10 * 65536) / 128, -32767, 32767); // метры → Q8.8 ячеек
                    const auto i = static_cast<std::size_t>(local_index(x, y, z));
                    chunk.distance[i] = static_cast<std::int16_t>(d);
                    chunk.material[i] = d < 0 ? Rock : Air;
                }
    }
};

int main() {
    const Layout small{.chunks_x = 2, .chunks_y = 1, .chunks_z = 3}; // 32 × 16 × 48 м
    SdfWorld flat(small, std::make_unique<FlatSource>());
    SdfWorld hills(1); // рядом — обычный мир 8×4×8: в одном процессе их сколько угодно
    std::printf("1. плоский мир %d чанков, холмы %d чанков\n", flat.layout().chunk_count(), hills.layout().chunk_count());
    EXPECT(flat.layout().chunk_count() == 6 && hills.layout().chunk_count() == 256);

    // 2. Тот же API, что у любого мира: sample, правки, сетки. Стены мира — по его размеру.
    const WorldPos p = WorldPos::from_meters(5, 14, 5);
    std::printf("2. над плоскостью на 4 м: sample = %+.1f; за краем мира по x (33 м > 32 м) — порода: %+.1f\n", flat.sample(p).to_double(),
                flat.sample(WorldPos::from_meters(33, 14, 5)).to_double());
    EXPECT(flat.sample(p) == Fixed::from_int(4));
    EXPECT(flat.sample(WorldPos::from_meters(33, 14, 5)).raw < 0);

    const EditResult r = flat.carve_sphere(WorldPos::from_meters(16, 10, 24), Fixed::from_int(3));
    for (const ChunkCoord c : r.chunks) EXPECT(small.contains(c)); // правка не выходит за форму мира
    EXPECT(flat.sample(WorldPos::from_meters(16, 10, 24)).raw > 0);
    const ChunkMesh mesh = mesh_chunk(flat, {0, 0, 1});
    std::printf("3. вырезали яму: затронуто %zu чанков, сетка чанка (0,0,1): %zu треугольников\n", r.chunks.size(), mesh.triangle_count());
    EXPECT(mesh.triangle_count() > 0);

    // 4. Для рендера и физики мир — просто Math::SdfField: любой код, умеющий работать с полем, подходит и этому миру.
    const Math::SdfField& field = flat;
    EXPECT(Math::raycast(field, WorldPos::from_meters(5, 15, 5), {Fixed{}, Fixed::from_int(-1), Fixed{}}, Fixed::from_int(10)).has_value());
    std::printf("4. луч по Math::SdfField попал в плоскую землю\nOK\n");
    return 0;
}
