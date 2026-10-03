/**
 * @example 01_dig_and_raycast.cpp
 * Ландшафт из знакового поля расстояний (SDF): запросы, луч, правки сферой, очередь перестройки сеток, хеш.
 *
 * Мир 128×64×128 м в чанках 32³ (ячейка 0,5 м); генерируется из сида — один сид, один и тот же мир.
 */

#include <Terrain/Mesher.hpp>

#include <cstdio>

using namespace Terrain;
using Math::Fixed;
using Math::FVec3;
using Math::WorldPos;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    // 1. Мир из сида. Сразу после создания все чанки «грязные»: сетки ещё никто не строил.
    SdfWorld world(/*seed=*/7);
    std::printf("1. мир: %d×%d×%d чанков (%d), в очереди на меш: %zu\n", world.layout().chunks_x, world.layout().chunks_y, world.layout().chunks_z,
                world.layout().chunk_count(), world.dirty_count());
    EXPECT(world.dirty_count() == 256);
    EXPECT(SdfWorld(7).hash() == world.hash()); // один сид — один мир, побитово

    // 2. sample: расстояние до поверхности в метрах (< 0 — внутри породы). Высоту земли над колонкой даёт ground_height.
    const WorldPos column = WorldPos::from_meters(64, 0, 64);
    const std::int64_t ground = world.ground_height(column);
    const WorldPos above{column.x, ground + WorldPos::from_meters(0, 3, 0).y, column.z};
    const WorldPos below{column.x, ground - WorldPos::from_meters(0, 3, 0).y, column.z};
    std::printf("2. земля над (64, 64) на высоте %.2f м; на 3 м выше sample = %+.2f, на 3 м ниже = %+.2f\n", static_cast<double>(ground) / 65536.0,
                world.sample(above).to_double(), world.sample(below).to_double());
    EXPECT(world.sample(above).raw > 0 && world.sample(below).raw < 0);

    // 3. Луч (шагает на длину расстояния до поверхности): так заклинания находят точку прицела.
    const WorldPos sky = WorldPos::from_meters(64, 62, 64);
    const auto hit = world.raycast(sky, FVec3{Fixed{}, Fixed::from_int(-1), Fixed{}}, Fixed::from_int(100));
    EXPECT(hit.has_value());
    std::printf("3. луч вниз с высоты 62 м попал в землю на %.2f м ниже\n", hit->distance.to_double());

    // 4. Правка: вырезать сферу радиуса 3 м в точке попадания. Возвращает границы изменений и затронутые чанки.
    (void)world.take_dirty(); // очередь первичных сеток разобрали
    const EditResult edit = world.carve_sphere(hit->position, Fixed::from_int(3));
    std::printf("4. вырезали шар: затронуто %zu чанков, отсчёты x %d…%d; в центре теперь sample = %+.2f (воздух)\n", edit.chunks.size(), edit.bounds.lo[0],
                edit.bounds.hi[0], world.sample(hit->position).to_double());
    EXPECT(edit.changed());
    EXPECT(world.sample(hit->position).raw > 0);

    // 5. Очередь перестройки: правка поставила в неё ровно затронутые чанки (с соседями, читающими край).
    const std::vector<ChunkCoord> dirty = world.take_dirty();
    EXPECT(dirty.size() == edit.chunks.size());
    std::printf("5. в очереди на перестройку: %zu чанков; после take_dirty очередь пуста: %s\n", dirty.size(), world.dirty_count() == 0 ? "да" : "нет");

    // 6. Сетка поверхности (marching tetrahedra). Вершины на общей грани соседних чанков совпадают — швов нет.
    ChunkMesh mesh;
    mesh_chunk(world, dirty.front(), mesh); // в готовый буфер: память переиспользуется между перестройками
    std::printf("6. сетка чанка (%d,%d,%d): %zu вершин, %zu треугольников\n", dirty.front().x, dirty.front().y, dirty.front().z, mesh.vertices.size(), mesh.triangle_count());

    // 7. Хеш всех отсчётов: два мира с одним сидом и одним списком правок совпадают — основа повторов.
    SdfWorld twin(7);
    (void)twin.carve_sphere(hit->position, Fixed::from_int(3));
    std::printf("7. хеш мира %016llx, у близнеца после той же правки %016llx\n", static_cast<unsigned long long>(world.hash()), static_cast<unsigned long long>(twin.hash()));
    EXPECT(world.hash() == twin.hash());
    std::printf("OK\n");
    return 0;
}
