#include <ManaField/ManaField.hpp>

#include <Math/Assert.hpp>
#include <Math/Hash.hpp>

#include <algorithm>

namespace ManaField {

using Math::Fixed;
using Math::Mana;
using Math::WorldPos;

ManaGrid::ManaGrid(const Config& config) : m_config(config) {
    FLUX_ASSERT(config.diffusion <= Fixed::from_ratio(1, 6), "ManaField: коэффициент диффузии > 1/6 неустойчив");
}

ManaGrid::Chunk& ManaGrid::ensure(const ChunkKey& key) {
    auto it = m_chunks.find(key);
    if (it == m_chunks.end()) {
        auto chunk = std::make_unique<Chunk>();
        chunk->cur.fill(m_config.base.value);
        it = m_chunks.emplace(key, std::move(chunk)).first;
    }
    return *it->second;
}

Fixed& ManaGrid::cell_ref(CellPos cell) {
    Chunk& chunk = ensure(chunk_of(cell));
    const auto local = [](std::int64_t c) { return static_cast<int>(c - floor_div(c, chunk_cells) * chunk_cells); };
    return chunk.cur[local_index(local(cell.x), local(cell.y), local(cell.z))];
}

Mana ManaGrid::at(CellPos cell) const noexcept {
    const auto it = m_chunks.find(chunk_of(cell));
    if (it == m_chunks.end()) return m_config.base; // виртуальная база
    const auto local = [](std::int64_t c) { return static_cast<int>(c - floor_div(c, chunk_cells) * chunk_cells); };
    return Mana(it->second->cur[local_index(local(cell.x), local(cell.y), local(cell.z))]);
}

bool ManaGrid::settled(const Chunk& chunk) const noexcept {
    return std::ranges::all_of(chunk.cur, [&](Fixed c) { return c == m_config.base.value; });
}

void ManaGrid::step() {
    if (m_chunks.empty()) return; // всё в базе — шагать нечего
    // Гало: соседи активных чанков по граням принимают поток; они начинают с базы и удаляются, если поток не дошёл.
    std::vector<ChunkKey> keys;
    keys.reserve(m_chunks.size());
    for (const auto& [key, chunk] : m_chunks) keys.push_back(key);
    for (const ChunkKey& k : keys) {
        for (const ChunkKey& n : {ChunkKey{k.x - 1, k.y, k.z}, ChunkKey{k.x + 1, k.y, k.z}, ChunkKey{k.x, k.y - 1, k.z}, ChunkKey{k.x, k.y + 1, k.z},
                                  ChunkKey{k.x, k.y, k.z - 1}, ChunkKey{k.x, k.y, k.z + 1}}) {
            ensure(n);
        }
    }

    for (auto& [key, chunk] : m_chunks) chunk->next = chunk->cur;
    const std::int64_t d = m_config.diffusion.raw;
    // Поток по паре считается один раз (его считает чанк нижней ячейки) и применяется к обеим сторонам, в том числе через границу чанков.
    const auto exchange = [d](Chunk& a, std::size_t ia, Chunk& b, std::size_t ib) {
        const auto flow = static_cast<std::int32_t>((static_cast<std::int64_t>(a.cur[ia].raw) - b.cur[ib].raw) * d / Fixed::one_raw);
        a.next[ia].raw -= flow;
        b.next[ib].raw += flow;
    };
    const auto find = [&](ChunkKey k) -> Chunk* {
        const auto it = m_chunks.find(k);
        return it == m_chunks.end() ? nullptr : it->second.get();
    };
    for (auto& [key, owner] : m_chunks) {
        Chunk& c = *owner;
        Chunk* px = find({key.x + 1, key.y, key.z});
        Chunk* py = find({key.x, key.y + 1, key.z});
        Chunk* pz = find({key.x, key.y, key.z + 1});
        for (int y = 0; y < chunk_cells; ++y) {
            for (int z = 0; z < chunk_cells; ++z) {
                for (int x = 0; x < chunk_cells; ++x) {
                    const std::size_t i = local_index(x, y, z);
                    if (x + 1 < chunk_cells) exchange(c, i, c, local_index(x + 1, y, z));
                    else if (px) exchange(c, i, *px, local_index(0, y, z));
                    if (z + 1 < chunk_cells) exchange(c, i, c, local_index(x, y, z + 1));
                    else if (pz) exchange(c, i, *pz, local_index(x, y, 0));
                    if (y + 1 < chunk_cells) exchange(c, i, c, local_index(x, y + 1, z));
                    else if (py) exchange(c, i, *py, local_index(x, 0, z));
                }
            }
        }
    }

    // Возврат к базе: хотя бы на единицу за шаг, поэтому поле сходится ровно и чанки освобождаются.
    const std::int64_t r = m_config.relax.raw, base = m_config.base.value.raw;
    for (auto it = m_chunks.begin(); it != m_chunks.end();) {
        Chunk& c = *it->second;
        if (r != 0) {
            for (std::size_t i = 0; i < chunk_volume; ++i) {
                const std::int64_t diff = base - c.cur[i].raw;
                if (diff == 0) continue;
                std::int64_t delta = diff * r / Fixed::one_raw;
                if (delta == 0) delta = diff > 0 ? 1 : -1;
                c.next[i].raw += static_cast<std::int32_t>(delta);
            }
        }
        c.cur = c.next;
        it = settled(c) ? m_chunks.erase(it) : std::next(it);
    }
    ++m_version;
}

Mana ManaGrid::draw(WorldPos pos, Fixed radius, Mana amount_mana) {
    const Fixed amount = amount_mana.value;
    if (amount.raw <= 0 || radius.raw <= 0) return {};
    const std::int64_t r = radius.raw, r2 = r * r;
    const CellPos lo = cell_of({pos.x - r, pos.y - r, pos.z - r}), hi = cell_of({pos.x + r, pos.y + r, pos.z + r});
    const auto inside = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
        const std::int64_t dx = x * cell_raw + cell_raw / 2 - pos.x, dy = y * cell_raw + cell_raw / 2 - pos.y, dz = z * cell_raw + cell_raw / 2 - pos.z;
        return dx * dx + dy * dy + dz * dz <= r2;
    };
    std::int64_t available = 0;
    for (std::int64_t y = lo.y; y <= hi.y; ++y)
        for (std::int64_t z = lo.z; z <= hi.z; ++z)
            for (std::int64_t x = lo.x; x <= hi.x; ++x)
                if (inside(x, y, z)) available += at(x, y, z).raw();
    if (available <= 0) return {};

    const std::int64_t want = std::min<std::int64_t>(amount.raw, available);
    std::int64_t taken = 0;
    for (std::int64_t y = lo.y; y <= hi.y; ++y)
        for (std::int64_t z = lo.z; z <= hi.z; ++z)
            for (std::int64_t x = lo.x; x <= hi.x; ++x) {
                if (!inside(x, y, z)) continue;
                Fixed& cell = cell_ref({x, y, z});
                const auto part = static_cast<std::int32_t>(static_cast<std::int64_t>(cell.raw) * want / available);
                cell.raw -= part;
                taken += part;
            }
    // Остаток от целочисленного деления (меньше числа ячеек единиц) добираем по порядку: отдаём ровно `want`.
    for (std::int64_t y = lo.y; y <= hi.y && taken < want; ++y)
        for (std::int64_t z = lo.z; z <= hi.z && taken < want; ++z)
            for (std::int64_t x = lo.x; x <= hi.x && taken < want; ++x) {
                if (!inside(x, y, z)) continue;
                Fixed& cell = cell_ref({x, y, z});
                const auto extra = static_cast<std::int32_t>(std::min<std::int64_t>(cell.raw, want - taken));
                cell.raw -= extra;
                taken += extra;
            }
    ++m_version;
    return Mana::from_raw(static_cast<std::int32_t>(taken));
}

void ManaGrid::inject(WorldPos pos, Mana amount) {
    if (amount.raw() <= 0) return;
    Fixed& cell = cell_ref(cell_of(pos));
    cell = cell + amount.value;
    ++m_version;
}

std::int64_t ManaGrid::excess_raw() const noexcept {
    std::int64_t sum = 0;
    for (const auto& [key, chunk] : m_chunks) {
        for (const Fixed c : chunk->cur) sum += static_cast<std::int64_t>(c.raw) - m_config.base.value.raw;
    }
    return sum;
}

std::uint64_t ManaGrid::hash() const {
    if (m_hashed_version != m_version) {
        Math::Hasher h;
        for (const auto& [key, chunk] : m_chunks) {
            if (settled(*chunk)) continue; // «пустые» чанки не влияют на хеш: он не зависит от истории выделения
            h.add_signed(key.x), h.add_signed(key.y), h.add_signed(key.z);
            h.add_span(std::span<const Fixed>(chunk->cur));
        }
        m_hash = h.value();
        m_hashed_version = m_version;
    }
    return m_hash;
}

} // namespace ManaField
