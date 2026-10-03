#pragma once
/**
 * @file Terrain.hpp
 * @brief Ландшафт: знаковое поле расстояний (SDF) в чанках 32³; размеры мира — данные (`Layout`), по умолчанию 8×4×8 чанков = 128×64×128 м.
 *
 * Данные:
 * - ячейка 0,5 м; отсчёт — int16 расстояние в Q8.8 (единица — ячейка) и uint8 материал, массивы раздельно (SoA);
 * - знак: внутри породы минус, в воздухе плюс, поверхность на нуле;
 * - каждый отсчёт принадлежит ровно одному чанку: сетке чанка недостающий слой читает у соседей, швов нет.
 *
 * Размер мира и источник чанков (`ChunkSource`) задаются при создании: в одном процессе могут жить несколько миров
 * (измерения, планеты, тесты) с разной формой и генерацией.
 *
 * Симуляция считает только в целых числах и Fixed. Генерация зависит только от источника (шум с сидом), правки —
 * сферы (`carve_sphere` — d = max(d, −s), `add_sphere` — d = min(d, s), s = |p − c| − r).
 * Столкновения персонажа считаются по `sample` и `gradient` напрямую, без физического движка.
 */

#include <Math/Fixed.hpp>
#include <Math/Sdf.hpp>
#include <Math/Vec.hpp>
#include <MemorySystem/MemorySystem.hpp>

#include <array>
#include <memory>
#include <cstdint>
#include <optional>
#include <vector>

namespace Terrain {

constexpr int chunk_size = 32;
constexpr int chunk_volume = chunk_size * chunk_size * chunk_size;
/// @brief Ячейка 0,5 м в единицах WorldPos.
constexpr std::int64_t cell_raw = Math::Fixed::one_raw / 2;

struct ChunkCoord {
    int x = 0, y = 0, z = 0;
    [[nodiscard]] friend constexpr bool operator==(ChunkCoord, ChunkCoord) noexcept = default;
};

/// @brief Форма мира: сколько чанков по осям. Всё остальное (отсчёты, размеры в метрах, номера чанков) — производные.
struct Layout {
    int chunks_x = 8, chunks_y = 4, chunks_z = 8;

    [[nodiscard]] constexpr int chunk_count() const noexcept { return chunks_x * chunks_y * chunks_z; }
    [[nodiscard]] constexpr int samples_x() const noexcept { return chunks_x * chunk_size; }
    [[nodiscard]] constexpr int samples_y() const noexcept { return chunks_y * chunk_size; }
    [[nodiscard]] constexpr int samples_z() const noexcept { return chunks_z * chunk_size; }
    /// @brief Размер мира в WorldPos (граница последнего отсчёта).
    [[nodiscard]] constexpr std::int64_t size_x() const noexcept { return (samples_x() - 1) * cell_raw; }
    [[nodiscard]] constexpr std::int64_t size_y() const noexcept { return (samples_y() - 1) * cell_raw; }
    [[nodiscard]] constexpr std::int64_t size_z() const noexcept { return (samples_z() - 1) * cell_raw; }
    [[nodiscard]] constexpr int index(ChunkCoord c) const noexcept { return (c.y * chunks_z + c.z) * chunks_x + c.x; }
    [[nodiscard]] constexpr ChunkCoord coord(int i) const noexcept { return {i % chunks_x, i / (chunks_x * chunks_z), (i / chunks_x) % chunks_z}; }
    [[nodiscard]] constexpr bool contains(ChunkCoord c) const noexcept {
        return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < chunks_x && c.y < chunks_y && c.z < chunks_z;
    }
    [[nodiscard]] friend constexpr bool operator==(const Layout&, const Layout&) noexcept = default;
};

/// @brief Материал отсчёта. Ноль — воздух (ZII); в пре-альфе одна порода.
enum Material : std::uint8_t { Air = 0, Rock = 1 };

/// @brief Данные чанка: SoA, нулевые байты — «воздух».
struct Chunk {
    std::array<std::int16_t, chunk_volume> distance; ///< Q8.8, в ячейках; < 0 — порода.
    std::array<std::uint8_t, chunk_volume> material;
};
static_assert(MemorySystem::ZeroInitializable<Chunk>);

/// @brief Индекс отсчёта внутри чанка.
[[nodiscard]] constexpr int local_index(int x, int y, int z) noexcept { return (y * chunk_size + z) * chunk_size + x; }

/// @brief Откуда берутся данные чанка при создании мира: шум, файл, сеть. Читает только свои аргументы (детерминированно).
class ChunkSource {
public:
    virtual ~ChunkSource() = default;
    /// @brief Заполняет обнулённый `chunk` (отсчёты чанка `coord` в координатах мира).
    virtual void generate(ChunkCoord coord, Chunk& chunk) const = 0;
};

/// @brief Холмы из шума высот с сидом: расстояние ≈ разность высот. Один сид — один и тот же мир.
class NoiseSource final : public ChunkSource {
public:
    explicit NoiseSource(std::uint64_t seed) : m_seed(seed) {}
    void generate(ChunkCoord coord, Chunk& chunk) const override;

private:
    std::uint64_t m_seed;
};

/// @brief Область отсчётов (включительно).
struct SampleBounds {
    int lo[3]{0, 0, 0};
    int hi[3]{-1, -1, -1};
    [[nodiscard]] bool empty() const noexcept { return hi[0] < lo[0] || hi[1] < lo[1] || hi[2] < lo[2]; }
};

/// @brief Итог правки: границы изменённых отсчётов и чанки, чьи сетки устарели (с соседями, читающими края).
struct EditResult {
    SampleBounds bounds;
    std::vector<ChunkCoord> chunks;
    [[nodiscard]] bool changed() const noexcept { return !chunks.empty(); }
};

using RayHit = Math::RayHit;

class SdfWorld final : public Math::SdfField {
public:
    /// @brief Мир-холмы из сида (`NoiseSource`): все чанки сгенерированы и помечены «грязными» (сетки ещё не строились).
    explicit SdfWorld(std::uint64_t seed, const Layout& layout = {});
    /// @brief Мир из любого источника чанков.
    SdfWorld(const Layout& layout, std::unique_ptr<ChunkSource> source);
    SdfWorld(SdfWorld&&) noexcept = default;
    SdfWorld& operator=(SdfWorld&&) noexcept = default;

    [[nodiscard]] const Layout& layout() const noexcept { return m_layout; }
    [[nodiscard]] const Chunk& chunk(ChunkCoord c) const noexcept { return *m_chunks[static_cast<std::size_t>(m_layout.index(c))]; }
    [[nodiscard]] std::uint32_t version(ChunkCoord c) const noexcept { return m_versions[static_cast<std::size_t>(m_layout.index(c))]; }

    /// @brief Расстояние отсчёта в метрах; индексы за краем мира зажимаются к краю.
    [[nodiscard]] Math::Fixed distance_at(int sx, int sy, int sz) const noexcept { return Math::Fixed::from_raw(raw_distance_at(sx, sy, sz) * 128); }
    /// @brief Формат хранения: Q8.8 в ячейках (единица — 1/256 ячейки). Нужен мешеру и сериализации; остальным — `distance_at`.
    [[nodiscard]] std::int16_t raw_distance_at(int sx, int sy, int sz) const noexcept;
    [[nodiscard]] std::uint8_t material_at(int sx, int sy, int sz) const noexcept;

    /**
     * @brief Расстояние до поверхности в метрах (трилинейная интерполяция отсчётов).
     * За краем мира: снизу и по бокам — порода (стены и дно удерживают персонажа), сверху — воздух.
     */
    [[nodiscard]] Math::Fixed sample(Math::WorldPos pos) const noexcept override;
    // Градиент SDF — из Math::SdfField (центральные разности по sample).
    /// @brief Луч по длине шага из расстояния (`Math::raycast` по этому полю); `dir` — единичный.
    [[nodiscard]] std::optional<RayHit> raycast(Math::WorldPos origin, Math::FVec3 dir, Math::Fixed max) const noexcept {
        return Math::raycast(*this, origin, dir, max);
    }
    /// @brief Высота поверхности над колонкой (x, z) в WorldPos.y: сверху вниз до первой породы (0, если столбец пуст).
    [[nodiscard]] std::int64_t ground_height(std::int64_t x, std::int64_t z) const noexcept;
    [[nodiscard]] std::int64_t ground_height(Math::WorldPos column) const noexcept { return ground_height(column.x, column.z); }

    /// @brief Вырезать сферу: d = max(d, −s).
    EditResult carve_sphere(Math::WorldPos center, Math::Fixed radius);
    /// @brief Насыпать сферу: d = min(d, s), материал — порода.
    EditResult add_sphere(Math::WorldPos center, Math::Fixed radius);

    /// @brief Хеш всех отсчётов (пересчитываются только изменённые чанки).
    [[nodiscard]] std::uint64_t hash() const;

    /// @brief Чанки, сетки которых нужно перестроить, без повторов; очередь очищается.
    [[nodiscard]] std::vector<ChunkCoord> take_dirty();
    [[nodiscard]] std::size_t dirty_count() const noexcept { return m_dirty_list.size(); }

private:
    EditResult edit(Math::WorldPos center, Math::Fixed radius, bool carve);
    void mark_dirty(ChunkCoord c);

    Layout m_layout;
    std::unique_ptr<ChunkSource> m_source;
    MemorySystem::Pool<Chunk> m_pool;
    std::vector<Chunk*> m_chunks;
    std::vector<std::uint32_t> m_versions;
    mutable std::vector<std::uint64_t> m_chunk_hash;
    mutable std::vector<std::uint32_t> m_hashed_version;
    std::vector<char> m_is_dirty;
    std::vector<ChunkCoord> m_dirty_list;
};

} // namespace Terrain
