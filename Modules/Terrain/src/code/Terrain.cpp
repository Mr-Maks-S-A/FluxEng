#include <Terrain/Terrain.hpp>

#include <Math/Assert.hpp>
#include <Math/Hash.hpp>
#include <Math/Noise.hpp>

#include <algorithm>
#include <limits>

namespace Terrain {

using Math::Fixed;
using Math::FVec3;
using Math::WorldPos;

namespace {

constexpr std::int64_t clamp64(std::int64_t v, std::int64_t lo, std::int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/// Q16.16 метры → Q8.8 ячейки (ячейка = 0,5 м: ×2 метров и ×256 дробных бит / 65536).
constexpr std::int64_t meters_to_cells_q8(std::int64_t raw) { return raw / 128; }
constexpr std::int16_t clamp_q8(std::int64_t v) { return static_cast<std::int16_t>(clamp64(v, -32767, 32767)); }

/// Высота поверхности (Q16.16 м) над (x, z) в метрах Fixed: только сид.
Fixed height_at(std::uint64_t seed, std::int64_t x_raw, std::int64_t z_raw) {
    const Fixed x = Fixed::saturate(x_raw / 24), z = Fixed::saturate(z_raw / 24); // шум с периодом 24 м
    const Fixed h = Fixed::from_int(12) + Math::fbm(seed, x, z, 4) * Fixed::from_int(26);
    return h;
}

} // namespace

void NoiseSource::generate(ChunkCoord c, Chunk& chunk) const {
    const std::uint64_t seed = m_seed;
    for (int z = 0; z < chunk_size; ++z) {
        const std::int64_t wz = static_cast<std::int64_t>(c.z * chunk_size + z) * cell_raw;
        for (int x = 0; x < chunk_size; ++x) {
            const std::int64_t wx = static_cast<std::int64_t>(c.x * chunk_size + x) * cell_raw;
            const std::int64_t h = height_at(seed, wx, wz).raw;
            for (int y = 0; y < chunk_size; ++y) {
                const std::int64_t wy = static_cast<std::int64_t>(c.y * chunk_size + y) * cell_raw;
                const std::int16_t d = clamp_q8(meters_to_cells_q8(wy - h)); // разность высот ≈ расстояние
                const int i = local_index(x, y, z);
                chunk.distance[static_cast<std::size_t>(i)] = d;
                chunk.material[static_cast<std::size_t>(i)] = d < 0 ? Rock : Air;
            }
        }
    }
}


SdfWorld::SdfWorld(std::uint64_t seed, const Layout& layout) : SdfWorld(layout, std::make_unique<NoiseSource>(seed)) {}

SdfWorld::SdfWorld(const Layout& layout, std::unique_ptr<ChunkSource> source)
    : m_layout(layout),
      m_source(std::move(source)),
      m_pool(MemorySystem::Pool<Chunk>::reserve(static_cast<std::size_t>(layout.chunk_count()), MemorySystem::KiB(256), MemorySystem::MemoryTag::Game)) {
    FLUX_ASSERT(layout.chunk_count() > 0, "Terrain: мир без чанков");
    const auto count = static_cast<std::size_t>(layout.chunk_count());
    m_chunks.assign(count, nullptr);
    m_versions.assign(count, 1);
    m_chunk_hash.assign(count, 0);
    m_hashed_version.assign(count, 0);
    m_is_dirty.assign(count, 0);
    for (int i = 0; i < layout.chunk_count(); ++i) {
        Chunk* chunk = m_pool.allocate();
        FLUX_ASSERT(chunk != nullptr, "Terrain: пул чанков исчерпан");
        m_source->generate(layout.coord(i), *chunk);
        m_chunks[static_cast<std::size_t>(i)] = chunk;
        mark_dirty(layout.coord(i));
    }
}

void SdfWorld::mark_dirty(ChunkCoord c) {
    const auto i = static_cast<std::size_t>(m_layout.index(c));
    if (m_is_dirty[i]) return;
    m_is_dirty[i] = 1;
    m_dirty_list.push_back(c);
}

std::vector<ChunkCoord> SdfWorld::take_dirty() {
    std::vector<ChunkCoord> out;
    out.swap(m_dirty_list);
    for (const ChunkCoord c : out) m_is_dirty[static_cast<std::size_t>(m_layout.index(c))] = 0;
    return out;
}

std::int16_t SdfWorld::raw_distance_at(int sx, int sy, int sz) const noexcept {
    sx = std::clamp(sx, 0, m_layout.samples_x() - 1), sy = std::clamp(sy, 0, m_layout.samples_y() - 1), sz = std::clamp(sz, 0, m_layout.samples_z() - 1);
    const ChunkCoord c{sx / chunk_size, sy / chunk_size, sz / chunk_size};
    return chunk(c).distance[static_cast<std::size_t>(local_index(sx % chunk_size, sy % chunk_size, sz % chunk_size))];
}

std::uint8_t SdfWorld::material_at(int sx, int sy, int sz) const noexcept {
    sx = std::clamp(sx, 0, m_layout.samples_x() - 1), sy = std::clamp(sy, 0, m_layout.samples_y() - 1), sz = std::clamp(sz, 0, m_layout.samples_z() - 1);
    const ChunkCoord c{sx / chunk_size, sy / chunk_size, sz / chunk_size};
    return chunk(c).material[static_cast<std::size_t>(local_index(sx % chunk_size, sy % chunk_size, sz % chunk_size))];
}

Fixed SdfWorld::sample(WorldPos pos) const noexcept {
    // Зажимаем точку в решётку отсчётов, запоминая, насколько она вне мира.
    const std::int64_t cx = clamp64(pos.x, 0, m_layout.size_x()), cy = clamp64(pos.y, 0, m_layout.size_y()), cz = clamp64(pos.z, 0, m_layout.size_z());
    const std::int64_t ix = std::min<std::int64_t>(cx / cell_raw, m_layout.samples_x() - 2), iy = std::min<std::int64_t>(cy / cell_raw, m_layout.samples_y() - 2),
                       iz = std::min<std::int64_t>(cz / cell_raw, m_layout.samples_z() - 2);
    const std::int64_t fx = cx - ix * cell_raw, fy = cy - iy * cell_raw, fz = cz - iz * cell_raw; // 0…cell_raw (15 бит)
    const auto d = [&](int dx, int dy, int dz) -> std::int64_t {
        return raw_distance_at(static_cast<int>(ix) + dx, static_cast<int>(iy) + dy, static_cast<int>(iz) + dz);
    };
    // Трилинейно: каждый уровень умножает на cell_raw (2¹⁵), итог масштаба 2⁴⁵ < 2⁶³ при |d| < 2¹⁵.
    constexpr std::int64_t c = cell_raw;
    const auto lerp = [](std::int64_t a, std::int64_t b, std::int64_t t) { return a * (c - t) + b * t; };
    const std::int64_t x00 = lerp(d(0, 0, 0), d(1, 0, 0), fx), x10 = lerp(d(0, 1, 0), d(1, 1, 0), fx);
    const std::int64_t x01 = lerp(d(0, 0, 1), d(1, 0, 1), fx), x11 = lerp(d(0, 1, 1), d(1, 1, 1), fx);
    const std::int64_t y0 = x00 * (c - fy) + x10 * fy, y1 = x01 * (c - fy) + x11 * fy;
    const std::int64_t v = y0 * (c - fz) + y1 * fz;
    Fixed result = Fixed::saturate(v / (c * c * c / 128)); // Q8.8 ячеек → Q16.16 метров: ×128
    // Стены и дно: за краем мира — порода, глубина растёт с удалением.
    const std::int64_t out_x = std::max(-pos.x, pos.x - m_layout.size_x()), out_z = std::max(-pos.z, pos.z - m_layout.size_z()), out_y = -pos.y;
    const std::int64_t outside = std::max({out_x, out_z, out_y, std::int64_t{0}});
    if (outside > 0) result = Math::min(result, Fixed::saturate(-outside - 655)); // −1 см: чтобы знак был строго отрицательным
    return result;
}

std::int64_t SdfWorld::ground_height(std::int64_t x, std::int64_t z) const noexcept {
    for (std::int64_t y = m_layout.size_y(); y >= 0; y -= cell_raw / 4) {
        if (sample({x, y, z}).raw < 0) return y;
    }
    return 0;
}

EditResult SdfWorld::edit(WorldPos center, Fixed radius, bool carve) {
    EditResult result;
    if (radius.raw <= 0) return result;
    // Область правки: куб сферы + 2 ячейки запаса (там расстояние ещё меняется).
    const std::int64_t reach = radius.raw + 2 * cell_raw;
    const auto lo = [&](std::int64_t c, int n) { return static_cast<int>(std::clamp<std::int64_t>(c - reach < 0 ? 0 : (c - reach) / cell_raw, 0, n - 1)); };
    const auto hi = [&](std::int64_t c, int n) { return static_cast<int>(std::clamp<std::int64_t>((c + reach) / cell_raw + 1, 0, n - 1)); };
    SampleBounds box;
    box.lo[0] = lo(center.x, m_layout.samples_x()), box.hi[0] = hi(center.x, m_layout.samples_x());
    box.lo[1] = lo(center.y, m_layout.samples_y()), box.hi[1] = hi(center.y, m_layout.samples_y());
    box.lo[2] = lo(center.z, m_layout.samples_z()), box.hi[2] = hi(center.z, m_layout.samples_z());
    if (center.x + reach < 0 || center.y + reach < 0 || center.z + reach < 0 || center.x - reach > m_layout.size_x() || center.y - reach > m_layout.size_y() ||
        center.z - reach > m_layout.size_z()) {
        return result; // сфера целиком вне мира
    }

    SampleBounds changed; // фактически изменённые отсчёты
    changed.lo[0] = changed.lo[1] = changed.lo[2] = std::numeric_limits<int>::max();
    changed.hi[0] = changed.hi[1] = changed.hi[2] = -1;
    for (int sy = box.lo[1]; sy <= box.hi[1]; ++sy) {
        for (int sz = box.lo[2]; sz <= box.hi[2]; ++sz) {
            for (int sx = box.lo[0]; sx <= box.hi[0]; ++sx) {
                const std::int64_t dx = sx * cell_raw - center.x, dy = sy * cell_raw - center.y, dz = sz * cell_raw - center.z;
                const std::int64_t dist = static_cast<std::int64_t>(Math::isqrt(static_cast<std::uint64_t>(dx * dx + dy * dy + dz * dz)));
                const std::int64_t s = meters_to_cells_q8(dist - radius.raw); // s = |p − c| − r, Q8.8 ячеек
                Chunk& chunk = *m_chunks[static_cast<std::size_t>(m_layout.index(ChunkCoord{sx / chunk_size, sy / chunk_size, sz / chunk_size}))];
                const auto i = static_cast<std::size_t>(local_index(sx % chunk_size, sy % chunk_size, sz % chunk_size));
                const std::int16_t old = chunk.distance[i];
                const std::int16_t now = clamp_q8(carve ? std::max<std::int64_t>(old, -s) : std::min<std::int64_t>(old, s));
                if (now == old) continue;
                chunk.distance[i] = now;
                chunk.material[i] = now < 0 ? Rock : Air;
                const int s3[3]{sx, sy, sz};
                for (int a = 0; a < 3; ++a) {
                    changed.lo[a] = std::min(changed.lo[a], s3[a]);
                    changed.hi[a] = std::max(changed.hi[a], s3[a]);
                }
            }
        }
    }
    if (changed.empty()) return result;
    result.bounds = changed;

    // Сетка чанка c читает отсчёты [c·32 − 1, c·32 + 33] (соседний слой для ячеек и нормалей).
    for (int cy = 0; cy < m_layout.chunks_y; ++cy)
        for (int cz = 0; cz < m_layout.chunks_z; ++cz)
            for (int cx = 0; cx < m_layout.chunks_x; ++cx) {
                const int c3[3]{cx, cy, cz};
                bool touches = true;
                for (int a = 0; a < 3; ++a) touches = touches && changed.lo[a] <= c3[a] * chunk_size + chunk_size + 1 && changed.hi[a] >= c3[a] * chunk_size - 1;
                if (!touches) continue;
                const ChunkCoord c{cx, cy, cz};
                ++m_versions[static_cast<std::size_t>(m_layout.index(c))];
                mark_dirty(c);
                result.chunks.push_back(c);
            }
    return result;
}

EditResult SdfWorld::carve_sphere(WorldPos center, Fixed radius) { return edit(center, radius, true); }
EditResult SdfWorld::add_sphere(WorldPos center, Fixed radius) { return edit(center, radius, false); }

void SdfWorld::load_chunk(ChunkCoord c, const Chunk& data) {
    FLUX_ASSERT(m_layout.contains(c), "SdfWorld::load_chunk: чанк вне мира");
    const auto i = static_cast<std::size_t>(m_layout.index(c));
    *m_chunks[i] = data;
    ++m_versions[i];
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const ChunkCoord n{c.x + dx, c.y + dy, c.z + dz};
                if (m_layout.contains(n)) mark_dirty(n); // сетка соседа читает край этого чанка
            }
}

std::uint64_t SdfWorld::hash() const {
    Math::Hasher total;
    for (std::size_t i = 0; i < m_chunks.size(); ++i) {
        if (m_hashed_version[i] != m_versions[i]) {
            Math::Hasher h;
            h.add_span(std::span<const std::int16_t>(m_chunks[i]->distance));
            h.add_span(std::span<const std::uint8_t>(m_chunks[i]->material));
            m_chunk_hash[i] = h.value();
            m_hashed_version[i] = m_versions[i];
        }
        total.add(m_chunk_hash[i]);
    }
    return total.value();
}

} // namespace Terrain
