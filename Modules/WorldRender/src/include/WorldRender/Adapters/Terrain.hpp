#pragma once
/**
 * @file Terrain.hpp
 * @brief Готовые источники данных для рендера: ландшафт `Terrain::SdfWorld` и поле маны `ManaField::ManaGrid`.
 *
 * Только этот заголовок знает про `Terrain` и `ManaField`; подключается цель `engine::WorldRenderTerrain`.
 *
 * @code
 * Terrain::SdfWorld world(1);
 * ManaField::ManaGrid mana;
 * WorldRender::TerrainSurface surface(world);                 // SurfaceSource для TerrainView
 * WorldRender::ManaFogSource fog(mana, world);                // FogSource для FogView
 * WorldRender::TerrainView terrain_view(device, surface);
 * terrain_view.enqueue(WorldRender::TerrainSurface::indices(world.take_dirty()));
 * @endcode
 */

#include <WorldRender/HeightMap.hpp>
#include <WorldRender/Sources.hpp>

#include <ManaField/ManaField.hpp>
#include <Terrain/Mesher.hpp>

#include <cstring>
#include <limits>
#include <span>

namespace WorldRender {

/// @brief Поверхность SDF-ландшафта (marching tetrahedra из модуля Terrain).
class TerrainSurface final : public SurfaceSource {
public:
    explicit TerrainSurface(const Terrain::SdfWorld& world) : m_world(&world) {}

    [[nodiscard]] GridShape shape() const override {
        const Terrain::Layout& l = m_world->layout();
        return {l.chunks_x, l.chunks_y, l.chunks_z, Terrain::chunk_size * 0.5, {0.0, 0.0, 0.0}};
    }

    void build(ChunkIndex chunk, SurfaceMesh& out) const override {
        static_assert(sizeof(Terrain::Vertex) == sizeof(SurfaceVertex), "вершины Terrain и рендера совпадают по раскладке");
        thread_local Terrain::ChunkMesh scratch; // у каждого потока JobSystem свой буфер: без аллокаций и блокировок
        Terrain::mesh_chunk(*m_world, {chunk.x, chunk.y, chunk.z}, scratch);
        out.vertices.resize(scratch.vertices.size());
        if (!scratch.vertices.empty()) std::memcpy(out.vertices.data(), scratch.vertices.data(), scratch.vertices.size() * sizeof(SurfaceVertex));
        out.indices = scratch.indices;
    }

    /// @brief Чанки ландшафта (`take_dirty`) → индексы рендера.
    [[nodiscard]] static std::vector<ChunkIndex> indices(std::span<const Terrain::ChunkCoord> chunks) {
        std::vector<ChunkIndex> out;
        out.reserve(chunks.size());
        for (const Terrain::ChunkCoord c : chunks) out.push_back({c.x, c.y, c.z});
        return out;
    }

private:
    const Terrain::SdfWorld* m_world;
};

/// @brief Высоты ландшафта для карты сверху: первая порода под колонкой (`SdfWorld::ground_height`). Пустой столбец — `NaN`.
class TerrainHeights final : public HeightSource {
public:
    explicit TerrainHeights(const Terrain::SdfWorld& world) : m_world(&world) {}

    [[nodiscard]] double height_at(double x, double z) const override {
        const std::int64_t h = m_world->ground_height(Math::WorldPos::from_doubles(x, 0.0, z));
        return h == 0 ? std::numeric_limits<double>::quiet_NaN() : static_cast<double>(h) / static_cast<double>(Math::Fixed::one_raw);
    }

    /// @brief Клетки карты под чанком ландшафта (чанк целиком по высоте: правка в любом слое меняет колонку).
    [[nodiscard]] static CellRegion cells_of_chunk(Terrain::ChunkCoord chunk, const HeightMap& map) {
        const double size = Terrain::chunk_size * 0.5;
        return map.cells_of(chunk.x * size, chunk.z * size, size);
    }

private:
    const Terrain::SdfWorld* m_world;
};

/// @brief Туман из поля маны: яркость = плотность / база. Приземный слой: без ячеек в породе и высоко над поверхностью.
class ManaFogSource final : public FogSource {
public:
    /// @param ground_layer Выше этого над поверхностью (м) туман не рисуется; ячейки глубже 1 м в породе — тоже.
    ManaFogSource(const ManaField::ManaGrid& field, const Terrain::SdfWorld& terrain, double ground_layer = 6.0)
        : m_field(&field), m_terrain(&terrain), m_layer(Math::Fixed::from_double(ground_layer)) {}

    [[nodiscard]] double cell_meters() const override { return 2.0; }

    [[nodiscard]] CellBox bounds() const override {
        const Terrain::Layout& l = m_terrain->layout(); // отсчёты ландшафта 0,5 м, ячейки поля 2 м
        return {{0, 0, 0}, {l.samples_x() / 4 - 1, l.samples_y() / 4 - 1, l.samples_z() / 4 - 1}};
    }

    [[nodiscard]] float density(std::int64_t x, std::int64_t y, std::int64_t z) const override {
        const double base = m_field->config().base.to_double();
        if (base <= 0.0) return 0.0f;
        // Один отсчёт ландшафта в центре ячейки (дёшево: десятки тысяч ячеек за кадр).
        const Math::Fixed sdf = m_terrain->distance_at(static_cast<int>((x * 2 + 1) * 2), static_cast<int>((y * 2 + 1) * 2), static_cast<int>((z * 2 + 1) * 2));
        if (sdf < Math::Fixed::from_int(-1) || sdf > m_layer) return 0.0f;
        return static_cast<float>(m_field->at(x, y, z).to_double() / base);
    }

private:
    const ManaField::ManaGrid* m_field;
    const Terrain::SdfWorld* m_terrain;
    Math::Fixed m_layer;
};

} // namespace WorldRender
