#pragma once
/**
 * @file Mesher.hpp
 * @brief Сетка поверхности чанка из SDF: marching tetrahedra, нормали из градиента, позиции в метрах от угла чанка.
 *
 * Куб ячейки делится на шесть тетраэдров вдоль главной диагонали (разбиение одинаково во всех ячейках, поэтому
 * диагонали граней совпадают у соседей), вершина лежит на ребре и считается одной функцией от двух отсчётов
 * на концах ребра — вершины на общей грани соседних чанков совпадают бит в бит, швов нет.
 *
 * Здесь float разрешён: сетка — только отображение, в состояние мира она не пишет.
 * Таблицы ячеек Transvoxel (обычные ячейки) подключатся заменой функции построения ячейки; переходные ячейки LOD
 * добавятся рядом. Лицензия таблиц Эрика Ленгиеля (MIT) проверяется при их подключении.
 */

#include <Terrain/Terrain.hpp>

#include <cstdint>
#include <vector>

namespace Terrain {

struct Vertex {
    float position[3];       ///< Метры от угла чанка (0…16): float не теряет точность вдали от начала мира.
    float normal[3];         ///< Единичная, наружу из породы.
    std::uint8_t material;
    std::uint8_t padding[3];
};
static_assert(sizeof(Vertex) == 28);

struct ChunkMesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices; ///< Тройки: против часовой стрелки, если смотреть снаружи.
    [[nodiscard]] std::size_t triangle_count() const noexcept { return indices.size() / 3; }
};

/// @brief Строит сетку чанка. Читает только `world` (константно) — безопасно вызывать из нескольких потоков.
[[nodiscard]] ChunkMesh mesh_chunk(const SdfWorld& world, ChunkCoord coord);
/// @brief То же в готовый буфер: `out` очищается, выделенная память переиспользуется (перестройка в цикле без аллокаций).
void mesh_chunk(const SdfWorld& world, ChunkCoord coord, ChunkMesh& out);

/// @brief Угол чанка в метрах (мировые координаты для float-отображения).
[[nodiscard]] inline constexpr double chunk_origin_meters(int chunk_index_on_axis) noexcept { return chunk_index_on_axis * chunk_size * 0.5; }

} // namespace Terrain
