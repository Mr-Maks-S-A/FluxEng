#pragma once
/**
 * @file Sources.hpp
 * @brief Откуда рендер берёт данные мира: интерфейсы вместо конкретных `Terrain` и `ManaField`.
 *
 * `WorldRender` рисует **то, что ему дали**: поверхность из чанков одинакового размера (`SurfaceSource`) и
 * туман из ячеек (`FogSource`). Что за ними стоит — SDF-ландшафт, воксели, сетка планеты, тест с тремя
 * треугольниками — рендеру безразлично. Благодаря этому:
 * - `WorldRender` не зависит от `Terrain` и `ManaField` (собирается и тестируется без них);
 * - планеты, LOD и другие миры подключаются новым источником, а не правкой рендера;
 * - готовые источники для ландшафта и поля маны — в `WorldRender/Adapters/Terrain.hpp` (цель `engine::WorldRenderTerrain`).
 *
 * @code
 * struct Quad final : WorldRender::SurfaceSource {                     // источник из одного чанка
 *     WorldRender::GridShape shape() const override { return {.chunks_x = 1, .chunks_y = 1, .chunks_z = 1, .chunk_meters = 16.0}; }
 *     void build(WorldRender::ChunkIndex, WorldRender::SurfaceMesh& out) const override { ... заполнить out ... }
 * };
 * Quad quad;
 * WorldRender::TerrainView view(device, quad);
 * view.enqueue(std::array{WorldRender::ChunkIndex{0, 0, 0}});
 * view.update(jobs, eye);          // строит меши (параллельно), грузит на GPU
 * view.draw(frame_view, light);
 * @endcode
 */

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace WorldRender {

/// @brief Номер чанка в сетке источника.
struct ChunkIndex {
    int x = 0, y = 0, z = 0;
    [[nodiscard]] friend constexpr bool operator==(ChunkIndex, ChunkIndex) noexcept = default;
};

/// @brief Сетка одинаковых кубических чанков: сколько их по осям, размер стороны и положение угла сетки в мире (метры).
struct GridShape {
    int chunks_x = 1, chunks_y = 1, chunks_z = 1;
    double chunk_meters = 16.0;
    double origin[3] = {0.0, 0.0, 0.0};

    [[nodiscard]] constexpr int chunk_count() const noexcept { return chunks_x * chunks_y * chunks_z; }
    [[nodiscard]] constexpr int index(ChunkIndex c) const noexcept { return (c.y * chunks_z + c.z) * chunks_x + c.x; }
    [[nodiscard]] constexpr ChunkIndex coord(int i) const noexcept { return {i % chunks_x, i / (chunks_x * chunks_z), (i / chunks_x) % chunks_z}; }
    [[nodiscard]] constexpr bool contains(ChunkIndex c) const noexcept {
        return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < chunks_x && c.y < chunks_y && c.z < chunks_z;
    }
    /// @brief Угол чанка в метрах мира (double: float теряет точность вдали от начала координат).
    [[nodiscard]] constexpr glm::dvec3 chunk_origin(ChunkIndex c) const noexcept {
        return {origin[0] + c.x * chunk_meters, origin[1] + c.y * chunk_meters, origin[2] + c.z * chunk_meters};
    }
    [[nodiscard]] friend constexpr bool operator==(const GridShape&, const GridShape&) noexcept = default;
};

/// @brief Вершина поверхности (28 байт): позиция в метрах **от угла чанка** (float не теряет точность), нормаль, материал.
struct SurfaceVertex {
    float position[3];
    float normal[3];
    std::uint8_t material;
    std::uint8_t padding[3];
};
static_assert(sizeof(SurfaceVertex) == 28, "раскладка вершины — часть контракта с шейдером");

struct SurfaceMesh {
    std::vector<SurfaceVertex> vertices;
    std::vector<std::uint32_t> indices; ///< Тройки; против часовой стрелки, если смотреть снаружи.
    [[nodiscard]] std::size_t triangle_count() const noexcept { return indices.size() / 3; }
    void clear() noexcept {
        vertices.clear();
        indices.clear();
    }
};

/// @brief Источник поверхности: сетка чанков и построение сетки одного чанка.
class SurfaceSource {
public:
    virtual ~SurfaceSource() = default;
    [[nodiscard]] virtual GridShape shape() const = 0;
    /**
     * @brief Строит сетку чанка в `out` (очищает её сам; память можно переиспользовать).
     * Вызывается из потоков JobSystem одновременно для разных чанков: реализация только **читает** мир.
     */
    virtual void build(ChunkIndex chunk, SurfaceMesh& out) const = 0;
};

/// @brief Ячейки тумана: индексы от `lo` до `hi` включительно.
struct CellBox {
    std::int64_t lo[3] = {0, 0, 0};
    std::int64_t hi[3] = {-1, -1, -1};
};

/// @brief Источник тумана: сетка кубических ячеек и яркость каждой.
class FogSource {
public:
    virtual ~FogSource() = default;
    [[nodiscard]] virtual double cell_meters() const = 0;
    /// @brief Где вообще есть что рисовать (ячейка (0, 0, 0) начинается в начале координат мира).
    [[nodiscard]] virtual CellBox bounds() const = 0;
    /// @brief Яркость ячейки: 1 — обычная плотность, меньше — «дыра», 0 — ничего не рисовать (породa, слишком высоко и т. п.).
    [[nodiscard]] virtual float density(std::int64_t x, std::int64_t y, std::int64_t z) const = 0;
};

} // namespace WorldRender
