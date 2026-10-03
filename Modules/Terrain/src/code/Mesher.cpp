#include <Terrain/Mesher.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Terrain {

namespace {

constexpr int pad = 1;                       // отсчёты вокруг чанка: [-1, 33]
constexpr int grid = chunk_size + 1 + 2 * pad; // 35
constexpr int lattice = chunk_size + 1;      // решётка вершин ячеек: 0…32

/// Локальная копия отсчётов вокруг чанка: нормали и ячейки читают её без обращений к соседним чанкам.
struct Grid {
    std::array<std::int16_t, grid * grid * grid> d;
    [[nodiscard]] std::int16_t at(int x, int y, int z) const noexcept {
        return d[static_cast<std::size_t>(((y + pad) * grid + (z + pad)) * grid + (x + pad))];
    }
};

/// Направления рёбер тетраэдров внутри куба: все ненулевые {0,1}³ → 7 типов.
constexpr int edge_code(int dx, int dy, int dz) { return (dx | (dy << 1) | (dz << 2)) - 1; }

/// Шесть тетраэдров куба: порядок осей вдоль пути от (0,0,0) к (1,1,1). Одинаков во всех ячейках.
constexpr std::array<std::array<int, 3>, 6> axis_orders = {{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};

struct Builder {
    const SdfWorld& world;
    ChunkCoord coord;
    Grid grid_data;
    std::vector<std::int32_t> edge_vertex; ///< [точка решётки][код ребра] → индекс вершины или −1.
    ChunkMesh mesh;
    std::array<int, 3> origin_samples;     ///< Глобальный индекс отсчёта (0,0,0) чанка.

    Builder(const SdfWorld& w, ChunkCoord c)
        : world(w), coord(c), edge_vertex(static_cast<std::size_t>(lattice) * lattice * lattice * 7, -1),
          origin_samples{c.x * chunk_size, c.y * chunk_size, c.z * chunk_size} {}

    void load() {
        for (int y = -pad; y < chunk_size + 1 + pad; ++y)
            for (int z = -pad; z < chunk_size + 1 + pad; ++z)
                for (int x = -pad; x < chunk_size + 1 + pad; ++x) {
                    // Ноль считаем «чуть снаружи»: вершина тогда лежит строго внутри ребра, а не в его конце, и вершины
                    // на общей грани соседних чанков образуются одними и теми же рёбрами.
                    const std::int16_t value = world.raw_distance_at(origin_samples[0] + x, origin_samples[1] + y, origin_samples[2] + z);
                    grid_data.d[static_cast<std::size_t>(((y + pad) * grid + (z + pad)) * grid + (x + pad))] = value == 0 ? std::int16_t{1} : value;
                }
    }

    [[nodiscard]] int d(int x, int y, int z) const noexcept { return grid_data.at(x, y, z); }

    /// Градиент поля в отсчёте: центральные разности (на краю мира отсчёты зажаты — нормаль плавно сходит к краю).
    void gradient(int x, int y, int z, float out[3]) const noexcept {
        out[0] = static_cast<float>(d(x + 1, y, z) - d(x - 1, y, z));
        out[1] = static_cast<float>(d(x, y + 1, z) - d(x, y - 1, z));
        out[2] = static_cast<float>(d(x, y, z + 1) - d(x, y, z - 1));
    }

    /// Вершина на ребре (a → b), a < b покомпонентно; создаётся один раз на ребро.
    std::uint32_t vertex_on_edge(int ax, int ay, int az, int dx, int dy, int dz) {
        const std::size_t slot = (static_cast<std::size_t>((az * lattice + ay) * lattice + ax)) * 7 + static_cast<std::size_t>(edge_code(dx, dy, dz));
        if (edge_vertex[slot] >= 0) return static_cast<std::uint32_t>(edge_vertex[slot]);
        const int bx = ax + dx, by = ay + dy, bz = az + dz;
        const float da = static_cast<float>(d(ax, ay, az)), db = static_cast<float>(d(bx, by, bz));
        const float t = da / (da - db); // da и db разного знака: da − db ≠ 0
        Vertex v{};
        constexpr float half = 0.5f; // метров на ячейку
        v.position[0] = (static_cast<float>(ax) + t * static_cast<float>(dx)) * half;
        v.position[1] = (static_cast<float>(ay) + t * static_cast<float>(dy)) * half;
        v.position[2] = (static_cast<float>(az) + t * static_cast<float>(dz)) * half;
        float ga[3], gb[3];
        gradient(ax, ay, az, ga);
        gradient(bx, by, bz, gb);
        float n[3];
        for (int i = 0; i < 3; ++i) n[i] = ga[i] + (gb[i] - ga[i]) * t;
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int i = 0; i < 3; ++i) v.normal[i] = len > 1e-6f ? n[i] / len : (i == 1 ? 1.0f : 0.0f);
        // Материал — у внутреннего (породного) конца ребра.
        const bool a_inside = da < 0.0f;
        v.material = world.material_at(origin_samples[0] + (a_inside ? ax : bx), origin_samples[1] + (a_inside ? ay : by),
                                       origin_samples[2] + (a_inside ? az : bz));
        const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(v);
        edge_vertex[slot] = static_cast<std::int32_t>(index);
        return index;
    }

    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        // Лицевая сторона — наружу: геометрическая нормаль должна смотреть туда же, куда нормали вершин.
        const Vertex &va = mesh.vertices[a], &vb = mesh.vertices[b], &vc = mesh.vertices[c];
        float e1[3], e2[3];
        for (int i = 0; i < 3; ++i) e1[i] = vb.position[i] - va.position[i], e2[i] = vc.position[i] - va.position[i];
        const float cross[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float facing = 0.0f;
        for (int i = 0; i < 3; ++i) facing += cross[i] * (va.normal[i] + vb.normal[i] + vc.normal[i]);
        if (facing < 0.0f) std::swap(b, c);
        mesh.indices.insert(mesh.indices.end(), {a, b, c});
    }

    void cell(int cx, int cy, int cz) {
        // Быстрый выход: все восемь углов одного знака.
        bool any_in = false, any_out = false;
        for (int i = 0; i < 8; ++i) (d(cx + (i & 1), cy + ((i >> 1) & 1), cz + ((i >> 2) & 1)) < 0 ? any_in : any_out) = true;
        if (!(any_in && any_out)) return;

        for (const auto& order : axis_orders) {
            int pos[3] = {cx, cy, cz};
            int corner[4][3];
            std::copy(pos, pos + 3, corner[0]);
            for (int step = 0; step < 3; ++step) {
                pos[order[static_cast<std::size_t>(step)]] += 1;
                std::copy(pos, pos + 3, corner[step + 1]); // corner[3] = (cx+1, cy+1, cz+1)
            }
            bool inside[4];
            int inside_count = 0;
            for (int k = 0; k < 4; ++k) {
                inside[k] = d(corner[k][0], corner[k][1], corner[k][2]) < 0;
                inside_count += inside[k];
            }
            if (inside_count == 0 || inside_count == 4) continue;

            const auto edge = [&](int i, int j) {
                // нижний конец ребра — покомпонентно меньший (углы тетраэдра растут вдоль пути)
                return vertex_on_edge(corner[i][0], corner[i][1], corner[i][2], corner[j][0] - corner[i][0], corner[j][1] - corner[i][1],
                                      corner[j][2] - corner[i][2]);
            };
            if (inside_count == 1 || inside_count == 3) {
                const bool lone_state = inside_count == 1; // «одинокая» вершина — в меньшинстве
                int lone = 0;
                for (int k = 0; k < 4; ++k) if (inside[k] == lone_state) lone = k;
                int others[3], n = 0;
                for (int k = 0; k < 4; ++k) if (k != lone) others[n++] = k;
                const auto e = [&](int other) { return lone < other ? edge(lone, other) : edge(other, lone); };
                triangle(e(others[0]), e(others[1]), e(others[2]));
            } else {
                int in[2], out[2], ni = 0, no = 0;
                for (int k = 0; k < 4; ++k) (inside[k] ? in[ni++] : out[no++]) = k;
                const auto e = [&](int i, int j) { return i < j ? edge(i, j) : edge(j, i); };
                const std::uint32_t v0 = e(in[0], out[0]), v1 = e(in[0], out[1]), v2 = e(in[1], out[1]), v3 = e(in[1], out[0]);
                triangle(v0, v1, v2);
                triangle(v0, v2, v3);
            }
        }
    }
};

} // namespace

void mesh_chunk(const SdfWorld& world, ChunkCoord coord, ChunkMesh& out) {
    Builder b(world, coord);
    b.mesh = std::move(out);
    b.mesh.vertices.clear();
    b.mesh.indices.clear();
    b.load();
    // Ячейка принадлежит чанку, в котором лежит её минимальный угол; в мире их последний слой — до последнего отсчёта.
    const int max_x = std::min(chunk_size, world.layout().samples_x() - 1 - coord.x * chunk_size);
    const int max_y = std::min(chunk_size, world.layout().samples_y() - 1 - coord.y * chunk_size);
    const int max_z = std::min(chunk_size, world.layout().samples_z() - 1 - coord.z * chunk_size);
    for (int y = 0; y < max_y; ++y)
        for (int z = 0; z < max_z; ++z)
            for (int x = 0; x < max_x; ++x) b.cell(x, y, z);
    out = std::move(b.mesh);
}

ChunkMesh mesh_chunk(const SdfWorld& world, ChunkCoord coord) {
    ChunkMesh mesh;
    mesh_chunk(world, coord, mesh);
    return mesh;
}

} // namespace Terrain
