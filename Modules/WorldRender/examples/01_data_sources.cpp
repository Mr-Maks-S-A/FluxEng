/**
 * @example 01_data_sources.cpp
 * Рендер не знает, что именно он рисует: данные приходят через `SurfaceSource` (поверхность из чанков) и `FogSource`
 * (туман из ячеек). Пример без окна и GPU: свои источники-заглушки, очередь перестройки сеток, выбор ячеек тумана, камеры.
 *
 * Готовые источники для ландшафта и поля маны — в `WorldRender/Adapters/Terrain.hpp` (см. следующий пример).
 */

#include <WorldRender/WorldRender.hpp>

#include <cstdio>

using namespace WorldRender;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/// Поверхность из одного квадрата в каждом чанке. Любой мир (воксели, планета) подключается так же: shape() и build().
struct QuadSurface final : SurfaceSource {
    GridShape shape() const override { return {.chunks_x = 3, .chunks_y = 1, .chunks_z = 3, .chunk_meters = 16.0, .origin = {1000.0, 0.0, 0.0}}; }
    void build(ChunkIndex, SurfaceMesh& out) const override { // вызывается из потоков JobSystem: только читает мир
        out.clear();
        for (const auto [x, z] : {std::pair{0.0f, 0.0f}, {16.0f, 0.0f}, {16.0f, 16.0f}, {0.0f, 16.0f}}) {
            out.vertices.push_back({{x, 0.0f, z}, {0.0f, 1.0f, 0.0f}, /*material*/ 1, {}}); // позиция — от угла чанка
        }
        out.indices = {0, 2, 1, 0, 3, 2};
    }
};

/// Туман: ячейка (3, 3, 3) — «дыра» (яркость 0.2), всё выше второго слоя скрыто.
struct HoleFog final : FogSource {
    double cell_meters() const override { return 2.0; }
    CellBox bounds() const override { return {{0, 0, 0}, {9, 9, 9}}; }
    float density(std::int64_t x, std::int64_t y, std::int64_t z) const override {
        if (y > 2) return 0.0f; // 0 — не рисовать
        return (x == 3 && y == 2 && z == 3) ? 0.2f : 1.0f;
    }
};

int main() {
    // 1. Источник описывает сетку чанков: размеры, сторона, положение в мире (origin — double: далёкие миры без потери точности).
    QuadSurface surface;
    const GridShape shape = surface.shape();
    std::printf("1. сетка %d×%d×%d чанков по %.0f м, угол чанка (2,0,1): (%.0f, %.0f, %.0f)\n", shape.chunks_x, shape.chunks_y, shape.chunks_z, shape.chunk_meters,
                shape.chunk_origin({2, 0, 1}).x, shape.chunk_origin({2, 0, 1}).y, shape.chunk_origin({2, 0, 1}).z);
    EXPECT(shape.chunk_origin({2, 0, 1}).x == 1032.0);

    // 2. Очередь перестройки: изменённые чанки без повторов, ближние к камере строятся первыми.
    MeshQueue queue(shape);
    queue.push({0, 0, 0});
    queue.push({2, 0, 2});
    queue.push({0, 0, 0}); // повтор игнорируется
    const auto next = queue.pop_nearest(1, glm::dvec3{1045.0, 5.0, 40.0}); // камера у дальнего угла
    std::printf("2. в очереди %zu, ближайший к камере: (%d,%d,%d)\n", queue.size() + 1, next[0].x, next[0].y, next[0].z);
    EXPECT((next[0] == ChunkIndex{2, 0, 2}));

    // 3. Источник строит сетку по запросу; рендер загружает её на GPU (здесь GPU нет — смотрим данные).
    SurfaceMesh mesh;
    surface.build({1, 0, 1}, mesh);
    std::printf("3. сетка чанка: %zu вершин, %zu треугольника\n", mesh.vertices.size(), mesh.triangle_count());
    EXPECT(mesh.triangle_count() == 2);

    // 4. Туман: для каждой ячейки в радиусе источник отвечает «насколько ярко»; ноль — скрыто.
    HoleFog fog;
    std::vector<FogVertex> vertices;
    const std::size_t cells = build_fog_vertices(fog, glm::dvec3{8.0, 4.0, 8.0}, 12.0f, vertices);
    float dimmest = 1.0f;
    for (const FogVertex& v : vertices) dimmest = std::min(dimmest, v.density);
    std::printf("4. ячеек тумана в радиусе 12 м: %zu (по 4 вершины), самая тусклая яркость %.1f — это «дыра»\n", cells, dimmest);
    EXPECT(cells > 20 && dimmest == 0.2f);

    // 5. Камеры работают с любой поверхностью (Math::SdfField): следящая не заходит в «землю», свободная летит по взгляду.
    const Math::PlaneSdf ground(Math::WorldPos::from_meters(0, 10, 0).y);
    CameraRig rig;
    rig.pitch = 1.2f; // смотрит вниз: камера оказалась бы под землёй
    const View view = rig.view({50.0, 11.6, 50.0}, ground, {1280.0f, 720.0f});
    std::printf("5. следящая камера прижата к цели: глаз на высоте %.2f м (земля 10), всё рисуется относительно глаза\n", view.eye.y);
    EXPECT(view.eye.y > 10.0 && glm::length(view.relative(view.eye)) == 0.0f);
    std::printf("OK\n");
    return 0;
}
