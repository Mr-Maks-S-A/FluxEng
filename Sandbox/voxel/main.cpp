/**
 * @file main.cpp
 * @brief Voxel — примитивный клон Minecraft: бесконечный воксельный мир на модулях движка.
 *
 * Цель — не игра, а **разведка**: какие части 3D-движка придётся выделить в модули.
 * Всё, чего в модулях нет, сделано прямо здесь и помечено `[в модуль]` — список собран
 * в `!TODO/CONSPECT_6.md`.
 *
 * Что берётся из модулей:
 * - **MemorySystem** — блоки чанков в `Pool<ChunkBlocks>`: обнулённый чанк = воздух (ZII);
 * - **JobSystem** — генерация и построение сеток чанков параллельно (`parallel_for`, кусок = чанк);
 * - **EventSystem** — правки блоков (`voxel.block_edit` → `voxel.block_changed`), загрузка/выгрузка чанков;
 * - **ECSSystem** — игрок и осколки разбитых блоков (Transform, Velocity, Debris);
 * - **Core / WindowSystem** — окно, фиксированный тик 60 Гц, клавиши → события, оверлей 2D;
 * - **RendererSystem** — только `Shader` и 2D-оверлей. 3D-рендер (сетки чанков на GPU, камера
 *   с перспективой, отсечение по пирамиде видимости, туман) — свой, в этом файле `[в модуль]`.
 *
 * Порядок тика:
 * 1. Controller — ходьба/полёт игрока, столкновения с блоками (AABB по осям);
 * 2. Interaction — луч из глаз (DDA): ЛКМ — сломать, ПКМ — поставить → `voxel.block_edit`;
 * 3. Terrain — **единственный владелец блоков**: применяет правки прошлого тика → `voxel.block_changed`,
 *    подгружает чанки вокруг игрока (∥ генерация) → `voxel.chunk_loaded`, выгружает дальние;
 * 4. Mesher — помечает «грязные» чанки по событиям и строит их сетки (∥, с затенением углов — AO);
 * 5. Debris — осколки сломанных блоков (сущности ECS) с гравитацией.
 *
 * Управление: мышь — обзор (Tab — отпустить/захватить курсор), WASD — ходьба, Space — прыжок,
 * F — полёт (Space/Shift — вверх/вниз), ЛКМ — сломать, ПКМ — поставить, 1–8 — блок, P — пауза.
 * Аргументы: `--radius N` — радиус прорисовки в чанках (по умолчанию 8), `--autopilot` — полёт по прямой
 * (замер подгрузки мира без ввода). Общие (`--ticks`, `--threads`, `--screenshot`) — см. Core::App.
 */

#include <Core/Core.hpp>
#include <ECSSystem/ECSSystem.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace es = EventSystem;
namespace ms = MemorySystem;
namespace js = JobSystem;
using namespace RendererSystem;

namespace {

// =============================================================================
// Блоки и чанки
// =============================================================================

constexpr int chunk_side = 16;
constexpr int chunk_height = 96;
constexpr int chunk_volume = chunk_side * chunk_side * chunk_height;

/// Тип блока. Ноль — воздух (ZII: обнулённый чанк — пустой).
enum Block : std::uint8_t { Air = 0, Grass, Dirt, Stone, Sand, Log, Leaves, Snow, Planks, Brick, BlockCount };

constexpr std::array<std::uint32_t, BlockCount> block_color = {
    0x00000000, 0x6DAA45FF, 0x8B5E3CFF, 0x8A8A8AFF, 0xDBCB8AFF, 0x6B4F2AFF, 0x3E7D2EFF, 0xF2F5F8FF, 0xB8915AFF, 0xA8483CFF};
constexpr std::array<std::string_view, BlockCount> block_name = {"air",    "grass", "dirt", "stone", "sand",
                                                                 "log",    "leaves", "snow", "planks", "brick"};
constexpr std::array<Block, 8> hotbar = {Grass, Dirt, Stone, Sand, Log, Leaves, Planks, Brick};

/// Блоки одного чанка: 16×16×96 байт. Индекс — x + z·16 + y·256.
struct ChunkBlocks {
    std::array<std::uint8_t, chunk_volume> blocks;
};
static_assert(ms::ZeroInitializable<ChunkBlocks>);

constexpr int block_index(int x, int y, int z) { return x + z * chunk_side + y * chunk_side * chunk_side; }

struct ChunkKey {
    std::int32_t x = 0;
    std::int32_t z = 0;
    friend bool operator==(ChunkKey, ChunkKey) = default;
    [[nodiscard]] std::int64_t packed() const { return (static_cast<std::int64_t>(x) << 32) | static_cast<std::uint32_t>(z); }
};
struct ChunkKeyHash {
    std::size_t operator()(ChunkKey k) const noexcept { return std::hash<std::int64_t>{}(k.packed()); }
};

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
ChunkKey chunk_of(int x, int z) { return {floor_div(x, chunk_side), floor_div(z, chunk_side)}; }

// =============================================================================
// Шум для генерации (детерминированный: только целочисленный хеш и float-арифметика)
// =============================================================================

std::uint32_t hash3(int x, int y, int z, std::uint32_t seed) {
    std::uint32_t h = seed ^ (static_cast<std::uint32_t>(x) * 0x8DA6B343U) ^ (static_cast<std::uint32_t>(y) * 0xD8163841U) ^
                      (static_cast<std::uint32_t>(z) * 0xCB1AB31FU);
    h ^= h >> 15;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12;
    h *= 0x297A2D39U;
    h ^= h >> 15;
    return h;
}
float unit(std::uint32_t h) { return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF); }
float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float value_noise2(float x, float z, std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x)), z0 = static_cast<int>(std::floor(z));
    const float tx = smooth(x - static_cast<float>(x0)), tz = smooth(z - static_cast<float>(z0));
    const float a = unit(hash3(x0, 0, z0, seed)), b = unit(hash3(x0 + 1, 0, z0, seed));
    const float c = unit(hash3(x0, 0, z0 + 1, seed)), d = unit(hash3(x0 + 1, 0, z0 + 1, seed));
    return std::lerp(std::lerp(a, b, tx), std::lerp(c, d, tx), tz);
}

float value_noise3(float x, float y, float z, std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y)), z0 = static_cast<int>(std::floor(z));
    const float tx = smooth(x - static_cast<float>(x0)), ty = smooth(y - static_cast<float>(y0)), tz = smooth(z - static_cast<float>(z0));
    float layer[2];
    for (int dy = 0; dy < 2; ++dy) {
        const float a = unit(hash3(x0, y0 + dy, z0, seed)), b = unit(hash3(x0 + 1, y0 + dy, z0, seed));
        const float c = unit(hash3(x0, y0 + dy, z0 + 1, seed)), d = unit(hash3(x0 + 1, y0 + dy, z0 + 1, seed));
        layer[dy] = std::lerp(std::lerp(a, b, tx), std::lerp(c, d, tx), tz);
    }
    return std::lerp(layer[0], layer[1], ty);
}

int terrain_height(int x, int z) {
    const float fx = static_cast<float>(x), fz = static_cast<float>(z);
    float h = 0.0f, amp = 1.0f, freq = 1.0f / 64.0f;
    for (int octave = 0; octave < 4; ++octave) {
        h += value_noise2(fx * freq, fz * freq, 17u + static_cast<std::uint32_t>(octave)) * amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    const float mountains = std::max(0.0f, value_noise2(fx / 180.0f, fz / 180.0f, 99u) - 0.55f) * 90.0f;
    return std::clamp(24 + static_cast<int>(h * 22.0f + mountains), 4, chunk_height - 12);
}

/// Генерация одного чанка: рельеф, пещеры (3D-шум), деревья. Пишет только свой чанк — можно параллельно.
void generate_chunk(ChunkKey key, ChunkBlocks& out) {
    const int base_x = key.x * chunk_side, base_z = key.z * chunk_side;
    for (int z = 0; z < chunk_side; ++z) {
        for (int x = 0; x < chunk_side; ++x) {
            const int wx = base_x + x, wz = base_z + z;
            const int h = terrain_height(wx, wz);
            const Block top = h < 27 ? Sand : (h > 62 ? Snow : Grass);
            for (int y = 0; y <= h; ++y) {
                Block b = y == h ? top : (y > h - 4 ? (top == Sand ? Sand : Dirt) : Stone);
                // Пещеры: «червоточины» в 3D-шуме, не ближе 3 блоков к поверхности.
                if (y > 2 && y < h - 3) {
                    const float n = value_noise3(static_cast<float>(wx) / 18.0f, static_cast<float>(y) / 12.0f,
                                                 static_cast<float>(wz) / 18.0f, 7u);
                    if (n > 0.72f) b = Air;
                }
                out.blocks[static_cast<std::size_t>(block_index(x, y, z))] = b;
            }
            // Дерево — по хешу колонки, целиком внутри чанка (чтобы чанк не писал в соседа).
            if (top == Grass && x >= 2 && x <= 13 && z >= 2 && z <= 13 && hash3(wx, 0, wz, 4242u) % 97 == 0 && h + 7 < chunk_height) {
                for (int y = h + 1; y <= h + 4; ++y) out.blocks[static_cast<std::size_t>(block_index(x, y, z))] = Log;
                for (int dy = 3; dy <= 6; ++dy) {
                    const int r = dy >= 5 ? 1 : 2;
                    for (int dz = -r; dz <= r; ++dz)
                        for (int dx = -r; dx <= r; ++dx) {
                            auto& cell = out.blocks[static_cast<std::size_t>(block_index(x + dx, h + dy, z + dz))];
                            if (cell == Air && !(std::abs(dx) == r && std::abs(dz) == r && dy != 4)) cell = Leaves;
                        }
                }
            }
        }
    }
}

// =============================================================================
// События
// =============================================================================

/// Игрок хочет поставить блок (Air — сломать). Применяет Terrain в следующем тике.
struct BlockEditEvent {
    std::int32_t x = 0, y = 0, z = 0;
    std::uint32_t block = 0;
    static constexpr std::string_view event_name = "voxel.block_edit";
    using fields = es::Fields<es::Field<"x", &BlockEditEvent::x>, es::Field<"y", &BlockEditEvent::y>,
                              es::Field<"z", &BlockEditEvent::z>, es::Field<"block", &BlockEditEvent::block>>;
};

/// Блок изменился (применённая правка).
struct BlockChangedEvent {
    std::int32_t x = 0, y = 0, z = 0;
    std::uint32_t old_block = 0;
    std::uint32_t new_block = 0;
    static constexpr std::string_view event_name = "voxel.block_changed";
    using fields = es::Fields<es::Field<"x", &BlockChangedEvent::x>, es::Field<"y", &BlockChangedEvent::y>,
                              es::Field<"z", &BlockChangedEvent::z>, es::Field<"old_block", &BlockChangedEvent::old_block>,
                              es::Field<"new_block", &BlockChangedEvent::new_block>>;
};

/// Чанк загружен (сгенерирован) или выгружен.
struct ChunkLoadedEvent {
    std::int32_t cx = 0, cz = 0;
    static constexpr std::string_view event_name = "voxel.chunk_loaded";
    using fields = es::Fields<es::Field<"cx", &ChunkLoadedEvent::cx>, es::Field<"cz", &ChunkLoadedEvent::cz>>;
};
struct ChunkUnloadedEvent {
    std::int32_t cx = 0, cz = 0;
    static constexpr std::string_view event_name = "voxel.chunk_unloaded";
    using fields = es::Fields<es::Field<"cx", &ChunkUnloadedEvent::cx>, es::Field<"cz", &ChunkUnloadedEvent::cz>>;
};

// =============================================================================
// Компоненты
// =============================================================================

struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 previous{0.0f}; ///< Позиция прошлого тика — для интерполяции камеры между тиками.
};
struct Velocity {
    glm::vec3 value{0.0f};
};
struct PlayerState {
    bool on_ground = false;
    bool flying = false;
};
struct Debris {
    float life = 0.0f;
    std::uint32_t block = 0;
};

constexpr float player_half_width = 0.3f;
constexpr float player_height = 1.8f;
constexpr float eye_height = 1.62f;

// =============================================================================
// Profiler
// =============================================================================

struct Profiler {
    enum Section : std::size_t { Controller, Interaction, Terrain, Mesher, Debris, Upload, Render, Count };
    static constexpr std::array<std::string_view, Count> names = {"controller", "interaction", "terrain", "mesher",
                                                                  "debris",     "gpu upload",  "render 3d"};
    using Clock = std::chrono::steady_clock;
    std::array<double, Count> total_ms{};
    std::array<double, Count> last_ms{};
    std::uint64_t ticks = 0, frames = 0;

    template<typename Fn>
    void measure(Section s, Fn&& fn) {
        const auto start = Clock::now();
        fn();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        total_ms[s] += ms;
        last_ms[s] = ms;
    }
    [[nodiscard]] static double per(double sum, std::uint64_t n) { return n > 0 ? sum / static_cast<double>(n) : 0.0; }

    void render_overlay(Renderer2D& r, glm::vec2 viewport) const {
        static constexpr std::array<std::uint32_t, Count> colors = {0x4FC3F7FF, 0x81C784FF, 0xFFB74DFF, 0xE57373FF,
                                                                    0xBA68C8FF, 0xFFF176FF, 0xF06292FF};
        const glm::vec2 origin{viewport.x - 12.0f - static_cast<float>(Count) * 18.0f, viewport.y - 12.0f};
        r.fill_rect({{origin.x - 6.0f, origin.y - 206.0f}, {static_cast<float>(Count) * 18.0f + 12.0f, 212.0f}}, Color{0, 0, 0, 150}, 0);
        for (int ms = 2; ms <= 20; ms += 2) r.fill_rect({{origin.x - 6.0f, origin.y - static_cast<float>(ms) * 10.0f}, {4.0f, 1.0f}}, Colors::white, 1);
        for (std::size_t s = 0; s < Count; ++s) {
            const float h = std::max(std::min(static_cast<float>(last_ms[s]) * 10.0f, 200.0f), 1.0f);
            r.fill_rect({{origin.x + static_cast<float>(s) * 18.0f, origin.y - h}, {12.0f, h}}, Color::from_rgba(colors[s]), 2);
        }
    }
};

// =============================================================================
// Terrain — единственный владелец блоков
// =============================================================================

struct Terrain {
    es::EventReader<BlockEditEvent> edits;
    es::EventWriter<BlockChangedEvent> changed_out;
    es::EventWriter<ChunkLoadedEvent> loaded_out;
    es::EventWriter<ChunkUnloadedEvent> unloaded_out;

    ms::Pool<ChunkBlocks> pool = ms::Pool<ChunkBlocks>::reserve(16'384);
    std::unordered_map<ChunkKey, ChunkBlocks*, ChunkKeyHash> chunks;
    int radius = 8;
    int budget_per_tick = 12;         ///< Сколько чанков генерировать за тик (остальные — в следующих).
    std::uint64_t generated = 0;
    std::uint64_t unloaded = 0;
    std::size_t peak_loaded = 0;
    std::vector<std::pair<ChunkKey, ChunkBlocks*>> pending; // память переиспользуется между тиками

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Terrain")
                                    .consumes<BlockEditEvent>()
                                    .produces<BlockChangedEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 4096})
                                    .produces<ChunkLoadedEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 8192})
                                    .produces<ChunkUnloadedEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 8192});
        edits = bus.reader<BlockEditEvent>(id);
        changed_out = bus.writer<BlockChangedEvent>(id);
        loaded_out = bus.writer<ChunkLoadedEvent>(id);
        unloaded_out = bus.writer<ChunkUnloadedEvent>(id);
    }

    [[nodiscard]] const ChunkBlocks* find(ChunkKey key) const {
        const auto it = chunks.find(key);
        return it == chunks.end() ? nullptr : it->second;
    }

    /// Блок в мировых координатах. Незагруженный чанк — `fallback` (для сеток — «твёрдо», чтобы не рисовать стенку).
    [[nodiscard]] std::uint8_t block_at(int x, int y, int z, std::uint8_t fallback = Air) const {
        if (y < 0) return Stone;
        if (y >= chunk_height) return Air;
        const ChunkBlocks* c = find(chunk_of(x, z));
        if (!c) return fallback;
        return c->blocks[static_cast<std::size_t>(block_index(x - floor_div(x, chunk_side) * chunk_side, y, z - floor_div(z, chunk_side) * chunk_side))];
    }
    [[nodiscard]] bool solid(int x, int y, int z) const { return block_at(x, y, z, Stone) != Air; }

    void tick(js::Scheduler& jobs, glm::vec3 center, bool unlimited = false) {
        apply_edits();
        stream(jobs, center, unlimited);
    }

    void apply_edits() {
        for (const BlockEditEvent& e : edits.events()) {
            if (e.y < 0 || e.y >= chunk_height) continue;
            const ChunkKey key = chunk_of(e.x, e.z);
            const auto it = chunks.find(key);
            if (it == chunks.end()) continue;
            auto& cell = it->second->blocks[static_cast<std::size_t>(block_index(e.x - key.x * chunk_side, e.y, e.z - key.z * chunk_side))];
            if (cell == e.block) continue;
            changed_out.emit(BlockChangedEvent{.x = e.x, .y = e.y, .z = e.z, .old_block = cell, .new_block = e.block});
            cell = static_cast<std::uint8_t>(e.block);
        }
    }

    void stream(js::Scheduler& jobs, glm::vec3 center, bool unlimited) {
        const ChunkKey c = chunk_of(static_cast<int>(std::floor(center.x)), static_cast<int>(std::floor(center.z)));

        // Выгрузка: дальше радиуса + 2 (запас, чтобы чанк не «мигал» на границе). Ключи сортируются —
        // порядок событий не зависит от порядка обхода unordered_map.
        std::vector<ChunkKey> far;
        for (const auto& [key, blocks] : chunks) {
            const int dx = key.x - c.x, dz = key.z - c.z;
            if (dx * dx + dz * dz > (radius + 2) * (radius + 2)) far.push_back(key);
        }
        std::ranges::sort(far, [](ChunkKey a, ChunkKey b) { return a.packed() < b.packed(); });
        for (const ChunkKey key : far) {
            pool.free(chunks[key]); // блок обнулён и вернулся в пул (ZII)
            chunks.erase(key);
            unloaded_out.emit(ChunkUnloadedEvent{.cx = key.x, .cz = key.z});
            ++unloaded;
        }

        // Загрузка: недостающие чанки в радиусе, ближние первыми.
        std::vector<ChunkKey> missing;
        for (int dz = -radius; dz <= radius; ++dz)
            for (int dx = -radius; dx <= radius; ++dx)
                if (dx * dx + dz * dz <= radius * radius && !chunks.contains({c.x + dx, c.z + dz})) missing.push_back({c.x + dx, c.z + dz});
        std::ranges::sort(missing, [&](ChunkKey a, ChunkKey b) {
            const int da = (a.x - c.x) * (a.x - c.x) + (a.z - c.z) * (a.z - c.z);
            const int db = (b.x - c.x) * (b.x - c.x) + (b.z - c.z) * (b.z - c.z);
            return da != db ? da < db : a.packed() < b.packed();
        });
        if (!unlimited && missing.size() > static_cast<std::size_t>(budget_per_tick)) missing.resize(static_cast<std::size_t>(budget_per_tick));

        pending.clear();
        for (const ChunkKey key : missing) pending.emplace_back(key, pool.allocate()); // нулевые блоки = воздух
        // ∥ генерация: кусок = один чанк, пишет только в свои блоки.
        js::parallel_for(jobs, pending.size(), 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) generate_chunk(pending[i].first, *pending[i].second);
        });
        for (const auto& [key, blocks] : pending) {
            chunks.emplace(key, blocks);
            loaded_out.emit(ChunkLoadedEvent{.cx = key.x, .cz = key.z});
        }
        generated += pending.size();
        peak_loaded = std::max(peak_loaded, chunks.size());
    }
};

// =============================================================================
// Mesher — сетки чанков (CPU), с затенением углов (ambient occlusion)
// =============================================================================

/// Вершина: позиция + цвет (оттенок блока × освещение грани × AO уже «запечены»). 16 байт.
struct Vertex {
    float x, y, z;
    std::uint32_t rgba;
};

struct ChunkMesh {
    std::vector<Vertex> vertices;
    std::uint32_t version = 0;   ///< Растёт при каждой перестройке: рендер по нему понимает, что пора загрузить на GPU.
    std::uint8_t neighbors = 0;  ///< Какие соседи (−x, +x, −z, +z) были загружены при постройке.
};

constexpr std::array<std::array<int, 2>, 4> side_offsets = {{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};

std::uint8_t loaded_neighbors(const Terrain& terrain, ChunkKey key) {
    std::uint8_t mask = 0;
    for (std::size_t i = 0; i < 4; ++i)
        if (terrain.find({key.x + side_offsets[i][0], key.z + side_offsets[i][1]})) mask |= static_cast<std::uint8_t>(1u << i);
    return mask;
}

std::uint32_t shade(std::uint32_t rgba, float k) {
    const auto ch = [&](int shift) {
        return static_cast<std::uint32_t>(std::clamp(static_cast<float>((rgba >> shift) & 0xFF) * k, 0.0f, 255.0f));
    };
    // Порядок байтов в памяти — R, G, B, A (атрибут читается как GL_UNSIGNED_BYTE ×4).
    return ch(24) | (ch(16) << 8) | (ch(8) << 16) | (0xFFu << 24);
}

void build_mesh(const Terrain& terrain, ChunkKey key, std::vector<Vertex>& out) {
    out.clear();
    const ChunkBlocks* self = terrain.find(key);
    if (!self) return;
    const int bx = key.x * chunk_side, bz = key.z * chunk_side;
    // Соседи по горизонтали ищутся один раз; внутри чанка — прямой индекс.
    const std::array<const ChunkBlocks*, 4> near = {terrain.find({key.x - 1, key.z}), terrain.find({key.x + 1, key.z}),
                                                    terrain.find({key.x, key.z - 1}), terrain.find({key.x, key.z + 1})};
    const auto at = [&](int x, int y, int z) -> std::uint8_t {
        if (y < 0) return Stone;
        if (y >= chunk_height) return Air;
        const ChunkBlocks* c = self;
        if (x < 0) c = near[0], x += chunk_side;
        else if (x >= chunk_side) c = near[1], x -= chunk_side;
        if (z < 0) c = (c == self ? near[2] : nullptr), z += chunk_side;
        else if (z >= chunk_side) c = (c == self ? near[3] : nullptr), z -= chunk_side;
        if (!c) return Stone; // сосед не загружен (или диагональ) — считаем твёрдым: стенку на краю мира не рисуем
        return c->blocks[static_cast<std::size_t>(block_index(x, y, z))];
    };

    static constexpr std::array<float, 6> face_light = {0.80f, 0.80f, 1.00f, 0.55f, 0.70f, 0.70f}; // ±x, ±y, ±z
    for (int y = 0; y < chunk_height; ++y) {
        for (int z = 0; z < chunk_side; ++z) {
            for (int x = 0; x < chunk_side; ++x) {
                const std::uint8_t b = self->blocks[static_cast<std::size_t>(block_index(x, y, z))];
                if (b == Air) continue;
                const int p[3] = {x, y, z};
                const float tint = 0.92f + 0.08f * unit(hash3(bx + x, y, bz + z, 1u)); // «текстура» без текстур
                for (int d = 0; d < 3; ++d) {
                    for (int s = -1; s <= 1; s += 2) {
                        int n[3] = {p[0], p[1], p[2]};
                        n[d] += s;
                        if (at(n[0], n[1], n[2]) != Air) continue; // грань закрыта
                        const int u = (d + 1) % 3, v = (d + 2) % 3;
                        // AO: для каждого угла грани — две боковые и угловая клетка в слое перед гранью.
                        std::array<float, 4> ao{};
                        std::array<std::array<float, 3>, 4> corner{};
                        static constexpr int cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1};
                        for (int k = 0; k < 4; ++k) {
                            int s1[3] = {n[0], n[1], n[2]}, s2[3] = {n[0], n[1], n[2]};
                            s1[u] += cu[k] ? 1 : -1;
                            s2[v] += cv[k] ? 1 : -1;
                            int cc[3] = {s1[0], s1[1], s1[2]};
                            cc[v] += cv[k] ? 1 : -1;
                            const int a = at(s1[0], s1[1], s1[2]) != Air, bb = at(s2[0], s2[1], s2[2]) != Air,
                                      c = at(cc[0], cc[1], cc[2]) != Air;
                            ao[static_cast<std::size_t>(k)] = (a && bb) ? 0.0f : static_cast<float>(3 - a - bb - c);
                            float q[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
                            q[d] += s > 0 ? 1.0f : 0.0f;
                            q[u] += static_cast<float>(cu[k]);
                            q[v] += static_cast<float>(cv[k]);
                            corner[static_cast<std::size_t>(k)] = {q[0] + static_cast<float>(bx), q[1], q[2] + static_cast<float>(bz)};
                        }
                        const float light = face_light[static_cast<std::size_t>(d * 2 + (s > 0 ? 0 : 1))] * tint;
                        const auto vert = [&](int k) {
                            const auto& q = corner[static_cast<std::size_t>(k)];
                            return Vertex{q[0], q[1], q[2], shade(block_color[b], light * (0.55f + 0.15f * ao[static_cast<std::size_t>(k)]))};
                        };
                        // Углы 0-1-2-3 обходятся против часовой, если смотреть с +d; для −d — обратный порядок.
                        // Диагональ выбирается по AO, чтобы затенение не «ломалось» по треугольникам.
                        std::array<int, 6> order = ao[0] + ao[2] >= ao[1] + ao[3] ? std::array<int, 6>{0, 1, 2, 0, 2, 3}
                                                                                  : std::array<int, 6>{1, 2, 3, 1, 3, 0};
                        if (s < 0) std::swap(order[1], order[2]), std::swap(order[4], order[5]);
                        for (const int k : order) out.push_back(vert(k));
                    }
                }
            }
        }
    }
}

struct Mesher {
    es::EventReader<ChunkLoadedEvent> loaded;
    es::EventReader<ChunkUnloadedEvent> unloaded;
    es::EventReader<BlockChangedEvent> changed;
    std::unordered_map<ChunkKey, ChunkMesh, ChunkKeyHash> meshes;
    std::vector<ChunkKey> dirty;
    int budget_per_tick = 16;
    std::uint64_t built = 0;
    std::uint64_t vertices_built = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id =
            bus.declare_module("Mesher").consumes<ChunkLoadedEvent>().consumes<ChunkUnloadedEvent>().consumes<BlockChangedEvent>();
        loaded = bus.reader<ChunkLoadedEvent>(id);
        unloaded = bus.reader<ChunkUnloadedEvent>(id);
        changed = bus.reader<BlockChangedEvent>(id);
    }

    void mark(const Terrain& terrain, ChunkKey key) {
        if (terrain.find(key) && std::ranges::find(dirty, key) == dirty.end()) dirty.push_back(key);
    }

    void tick(js::Scheduler& jobs, const Terrain& terrain, glm::vec3 center, bool unlimited = false) {
        for (const ChunkUnloadedEvent& e : unloaded.events()) meshes.erase({e.cx, e.cz});
        std::erase_if(dirty, [&](ChunkKey k) { return !terrain.find(k); });
        // Новый чанк: строим его сетку и перестраиваем соседей — но только тех, кто строился без него
        // (сетка помнит, какие соседи были при постройке). Так нет лишних перестроек.
        for (const ChunkLoadedEvent& e : loaded.events()) {
            const ChunkKey self{e.cx, e.cz};
            const auto mine = meshes.find(self);
            if (mine == meshes.end() || mine->second.neighbors != loaded_neighbors(terrain, self)) mark(terrain, self);
            for (std::size_t i = 0; i < 4; ++i) {
                const ChunkKey n{e.cx + side_offsets[i][0], e.cz + side_offsets[i][1]};
                const auto it = meshes.find(n);
                if (it != meshes.end() && it->second.neighbors != loaded_neighbors(terrain, n)) mark(terrain, n);
            }
        }
        for (const BlockChangedEvent& e : changed.events()) { // блок на краю чанка меняет и соседнюю сетку
            const ChunkKey k = chunk_of(e.x, e.z);
            const int lx = e.x - k.x * chunk_side, lz = e.z - k.z * chunk_side;
            mark(terrain, k);
            if (lx == 0) mark(terrain, {k.x - 1, k.z});
            if (lx == chunk_side - 1) mark(terrain, {k.x + 1, k.z});
            if (lz == 0) mark(terrain, {k.x, k.z - 1});
            if (lz == chunk_side - 1) mark(terrain, {k.x, k.z + 1});
        }

        // Ближние к игроку — первыми; правки блоков (они тоже ближние) не ждут.
        const ChunkKey c = chunk_of(static_cast<int>(std::floor(center.x)), static_cast<int>(std::floor(center.z)));
        std::ranges::sort(dirty, [&](ChunkKey a, ChunkKey b) {
            const int da = (a.x - c.x) * (a.x - c.x) + (a.z - c.z) * (a.z - c.z);
            const int db = (b.x - c.x) * (b.x - c.x) + (b.z - c.z) * (b.z - c.z);
            return da != db ? da < db : a.packed() < b.packed();
        });
        const std::size_t count = unlimited ? dirty.size() : std::min(dirty.size(), static_cast<std::size_t>(budget_per_tick));
        std::vector<ChunkMesh*> targets(count);
        for (std::size_t i = 0; i < count; ++i) targets[i] = &meshes[dirty[i]]; // вставки в map — до параллельной части
        // ∥ сетки: кусок = чанк, читает блоки (только чтение), пишет только свою сетку.
        js::parallel_for(jobs, count, 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                build_mesh(terrain, dirty[i], targets[i]->vertices);
                targets[i]->neighbors = loaded_neighbors(terrain, dirty[i]);
                ++targets[i]->version;
            }
        });
        for (std::size_t i = 0; i < count; ++i) vertices_built += targets[i]->vertices.size();
        built += count;
        dirty.erase(dirty.begin(), dirty.begin() + static_cast<std::ptrdiff_t>(count));
    }
};

// =============================================================================
// Луч из глаз: DDA по клеткам сетки
// =============================================================================

struct RayHit {
    glm::ivec3 block{0};
    glm::ivec3 before{0}; ///< Пустая клетка перед блоком — сюда ставится новый.
};

std::optional<RayHit> raycast(const Terrain& terrain, glm::vec3 origin, glm::vec3 dir, float max_distance) {
    glm::ivec3 cell{static_cast<int>(std::floor(origin.x)), static_cast<int>(std::floor(origin.y)), static_cast<int>(std::floor(origin.z))};
    const glm::ivec3 step{dir.x > 0 ? 1 : -1, dir.y > 0 ? 1 : -1, dir.z > 0 ? 1 : -1};
    glm::vec3 t_max, t_delta;
    for (int a = 0; a < 3; ++a) {
        const float next = static_cast<float>(cell[a]) + (step[a] > 0 ? 1.0f : 0.0f);
        t_delta[a] = dir[a] != 0.0f ? std::abs(1.0f / dir[a]) : 1e30f;
        t_max[a] = dir[a] != 0.0f ? (next - origin[a]) / dir[a] : 1e30f;
    }
    glm::ivec3 before = cell;
    for (float t = 0.0f; t <= max_distance;) {
        if (terrain.block_at(cell.x, cell.y, cell.z) != Air) return RayHit{cell, before};
        before = cell;
        const int a = t_max.x < t_max.y ? (t_max.x < t_max.z ? 0 : 2) : (t_max.y < t_max.z ? 1 : 2);
        t = t_max[a];
        t_max[a] += t_delta[a];
        cell[a] += step[a];
    }
    return std::nullopt;
}

glm::vec3 look_direction(float yaw, float pitch) {
    return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
}

// =============================================================================
// Controller — игрок: ввод → скорость → движение с столкновениями
// =============================================================================

/// Столкновение AABB с блоками: движение по осям по очереди, упор — по ближней грани блока.
bool move_axis(const Terrain& terrain, glm::vec3& pos, int axis, float delta) {
    pos[axis] += delta;
    const glm::vec3 lo = pos - glm::vec3{player_half_width, 0.0f, player_half_width};
    const glm::vec3 hi = pos + glm::vec3{player_half_width, player_height, player_half_width};
    for (int y = static_cast<int>(std::floor(lo.y)); y <= static_cast<int>(std::floor(hi.y - 1e-4f)); ++y)
        for (int z = static_cast<int>(std::floor(lo.z)); z <= static_cast<int>(std::floor(hi.z - 1e-4f)); ++z)
            for (int x = static_cast<int>(std::floor(lo.x)); x <= static_cast<int>(std::floor(hi.x - 1e-4f)); ++x) {
                if (!terrain.solid(x, y, z)) continue;
                const float cell_lo = static_cast<float>(axis == 0 ? x : axis == 1 ? y : z);
                const float extent_lo = axis == 1 ? 0.0f : player_half_width;
                const float extent_hi = axis == 1 ? player_height : player_half_width;
                pos[axis] = delta > 0 ? cell_lo - extent_hi - 1e-3f : cell_lo + 1.0f + extent_lo + 1e-3f;
                return true;
            }
    return false;
}

struct Controller {
    es::EventReader<Core::KeyEvent> keys;
    std::array<bool, GLFW_KEY_LAST + 1> held{}; ///< Состояние клавиш, собранное из событий (детерминированно).
    bool autopilot = false;
    float yaw = 0.0f;   ///< Обзор приходит из домена кадра (мышь), см. Voxel::render.
    float pitch = 0.0f;

    void declare(es::EventBus& bus) { keys = bus.reader<Core::KeyEvent>(bus.declare_module("Controller").consumes<Core::KeyEvent>()); }

    void tick(ECS::World& world, ECS::Entity player, const Terrain& terrain, float dt) {
        auto& state = *world.get<PlayerState>(player);
        for (const Core::KeyEvent& k : keys.events()) {
            if (k.key < 0 || k.key > GLFW_KEY_LAST) continue;
            if (k.action == GLFW_PRESS) held[static_cast<std::size_t>(k.key)] = true;
            if (k.action == GLFW_RELEASE) held[static_cast<std::size_t>(k.key)] = false;
            if (k.action == GLFW_PRESS && k.key == GLFW_KEY_F) state.flying = !state.flying;
        }
        Transform& t = *world.get<Transform>(player);
        glm::vec3& v = world.get<Velocity>(player)->value;
        t.previous = t.position;

        if (autopilot) { // полёт по прямой с покачиванием: подгрузка мира без ввода, детерминированно
            state.flying = true;
            yaw = 0.15f * std::sin(static_cast<float>(t.position.x) / 200.0f);
            pitch = -0.25f;
        }
        const glm::vec3 forward{std::cos(yaw), 0.0f, std::sin(yaw)};
        const glm::vec3 right{-forward.z, 0.0f, forward.x};
        glm::vec3 wish{0.0f};
        const auto down = [&](int key) { return held[static_cast<std::size_t>(key)]; };
        if (down(GLFW_KEY_W) || autopilot) wish += forward;
        if (down(GLFW_KEY_S)) wish -= forward;
        if (down(GLFW_KEY_D)) wish += right;
        if (down(GLFW_KEY_A)) wish -= right;
        if (glm::dot(wish, wish) > 0.0f) wish = glm::normalize(wish);

        if (state.flying) {
            const float speed = autopilot ? 24.0f : 12.0f;
            v = wish * speed;
            if (down(GLFW_KEY_SPACE)) v.y = speed;
            if (down(GLFW_KEY_LEFT_SHIFT)) v.y = -speed;
            if (autopilot) v.y = (48.0f + static_cast<float>(terrain_height(static_cast<int>(t.position.x), static_cast<int>(t.position.z))) * 0.5f - t.position.y) * 2.0f;
        } else {
            v.x = wish.x * 4.6f;
            v.z = wish.z * 4.6f;
            v.y = std::max(v.y - 26.0f * dt, -50.0f);
            if (state.on_ground && down(GLFW_KEY_SPACE)) v.y = 8.4f;
        }
        state.on_ground = false;
        for (int axis : {1, 0, 2}) {
            if (move_axis(terrain, t.position, axis, v[axis] * dt)) {
                if (axis == 1 && v.y < 0.0f) state.on_ground = true;
                v[axis] = 0.0f;
            }
        }
    }
};

// =============================================================================
// Interaction — сломать / поставить блок
// =============================================================================

struct Interaction {
    es::EventReader<Core::MouseButtonEvent> mouse;
    es::EventReader<Core::KeyEvent> keys;
    es::EventWriter<BlockEditEvent> out;
    std::size_t selected = 0;
    std::uint64_t broken = 0, placed = 0;
    bool autodig = false; ///< Автопилот: каждые 6 тиков ломает блок под собой (правки без ввода).

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Interaction")
                                    .consumes<Core::MouseButtonEvent>()
                                    .consumes<Core::KeyEvent>()
                                    .produces<BlockEditEvent>(es::ChannelConfig{.reserve = 16, .max_events_per_tick = 256});
        mouse = bus.reader<Core::MouseButtonEvent>(id);
        keys = bus.reader<Core::KeyEvent>(id);
        out = bus.writer<BlockEditEvent>(id);
    }

    void tick(const Terrain& terrain, glm::vec3 eye, glm::vec3 dir, glm::vec3 feet, bool captured, es::Tick now) {
        if (autodig && now % 6 == 0) {
            if (const std::optional<RayHit> hit = raycast(terrain, eye, {0.0f, -1.0f, 0.0f}, 96.0f)) {
                out.emit(BlockEditEvent{.x = hit->block.x, .y = hit->block.y, .z = hit->block.z, .block = Air});
                ++broken;
            }
        }
        for (const Core::KeyEvent& k : keys.events())
            if (k.action == GLFW_PRESS && k.key >= GLFW_KEY_1 && k.key <= GLFW_KEY_8) selected = static_cast<std::size_t>(k.key - GLFW_KEY_1);
        for (const Core::MouseButtonEvent& m : mouse.events()) {
            if (m.action != GLFW_PRESS || !captured) continue; // мышь в свободном режиме блоки не трогает
            const std::optional<RayHit> hit = raycast(terrain, eye, dir, 6.0f);
            if (!hit) continue;
            if (m.button == GLFW_MOUSE_BUTTON_LEFT) {
                out.emit(BlockEditEvent{.x = hit->block.x, .y = hit->block.y, .z = hit->block.z, .block = Air});
                ++broken;
            } else if (m.button == GLFW_MOUSE_BUTTON_RIGHT) {
                const glm::ivec3 b = hit->before;
                // Нельзя поставить блок в себя.
                const bool inside = static_cast<float>(b.x) + 1.0f > feet.x - player_half_width && static_cast<float>(b.x) < feet.x + player_half_width &&
                                    static_cast<float>(b.z) + 1.0f > feet.z - player_half_width && static_cast<float>(b.z) < feet.z + player_half_width &&
                                    static_cast<float>(b.y) + 1.0f > feet.y && static_cast<float>(b.y) < feet.y + player_height;
                if (inside) continue;
                out.emit(BlockEditEvent{.x = b.x, .y = b.y, .z = b.z, .block = hotbar[selected]});
                ++placed;
            }
        }
    }
};

// =============================================================================
// Debris — осколки сломанных блоков (единственный, кто создаёт и уничтожает сущности во время игры)
// =============================================================================

struct DebrisSystem {
    es::EventReader<BlockChangedEvent> changed;
    std::uint64_t spawned = 0;
    std::vector<ECS::Entity> expired;

    void declare(es::EventBus& bus) { changed = bus.reader<BlockChangedEvent>(bus.declare_module("Debris").consumes<BlockChangedEvent>()); }

    void tick(ECS::World& world, const Terrain& terrain, float dt) {
        expired.clear();
        world.view<Transform, Velocity, Debris>().each([&](ECS::Entity e, Transform& t, Velocity& v, Debris& d) {
            d.life -= dt;
            if (d.life <= 0.0f) {
                expired.push_back(e);
                return;
            }
            t.previous = t.position;
            v.value.y -= 20.0f * dt;
            glm::vec3 next = t.position + v.value * dt;
            if (terrain.solid(static_cast<int>(std::floor(next.x)), static_cast<int>(std::floor(next.y)), static_cast<int>(std::floor(next.z)))) {
                next = t.position;
                v.value *= glm::vec3{0.5f, -0.3f, 0.5f}; // отскок с потерей энергии
            }
            t.position = next;
        });
        for (const ECS::Entity e : expired) world.destroy(e);

        for (const BlockChangedEvent& c : changed.events()) {
            if (c.new_block != Air) continue;
            for (int i = 0; i < 8; ++i) {
                const std::uint32_t h = hash3(c.x, c.y, c.z, static_cast<std::uint32_t>(i) + 11u);
                const glm::vec3 offset{unit(h), unit(h >> 3), unit(h >> 6)};
                const glm::vec3 at = glm::vec3{c.x, c.y, c.z} + 0.2f + offset * 0.6f;
                const ECS::Entity e = world.create();
                world.emplace<Transform>(e, at, at);
                world.emplace<Velocity>(e, (offset - 0.5f) * glm::vec3{5.0f, 4.0f, 5.0f} + glm::vec3{0.0f, 3.0f, 0.0f});
                world.emplace<Debris>(e, 1.2f + unit(h >> 9) * 0.6f, c.old_block);
                ++spawned;
            }
        }
    }
};

// =============================================================================
// WorldRenderer — свой 3D-рендер [в модуль: RendererSystem 3D]
// =============================================================================

constexpr std::string_view voxel_vs = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
uniform mat4 u_view_projection;
uniform vec3 u_eye;
out vec4 v_color;
out float v_distance;
void main() {
    v_color = a_color;
    v_distance = length(a_position - u_eye);
    gl_Position = u_view_projection * vec4(a_position, 1.0);
}
)";

constexpr std::string_view voxel_fs = R"(#version 330 core
in vec4 v_color;
in float v_distance;
uniform vec3 u_fog_color;
uniform vec2 u_fog_range;
out vec4 frag_color;
void main() {
    float fog = smoothstep(u_fog_range.x, u_fog_range.y, v_distance);
    frag_color = vec4(mix(v_color.rgb, u_fog_color, fog), 1.0);
}
)";

/// Буфер вершин на GPU: VAO + VBO с раскладкой Vertex. [в модуль: RendererSystem::Mesh]
struct GpuMesh {
    GLuint vao = 0, vbo = 0;
    GLsizei count = 0;
    std::uint32_t version = 0;

    void upload(std::span<const Vertex> vertices, GLenum usage = GL_STATIC_DRAW) {
        if (vao == 0) {
            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, rgba)));
        }
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size_bytes()), vertices.data(), usage);
        glBindVertexArray(0);
        count = static_cast<GLsizei>(vertices.size());
    }
    void draw(GLenum mode = GL_TRIANGLES) const {
        if (count == 0) return;
        glBindVertexArray(vao);
        glDrawArrays(mode, 0, count);
    }
    void release() {
        if (vao) glDeleteVertexArrays(1, &vao), glDeleteBuffers(1, &vbo);
        vao = vbo = 0;
        count = 0;
    }
};

/// Шесть плоскостей пирамиды видимости из матрицы VP (метод Gribb–Hartmann). [в модуль: math / culling]
struct Frustum {
    std::array<glm::vec4, 6> planes{};
    explicit Frustum(const glm::mat4& m) {
        const glm::vec4 r0{m[0][0], m[1][0], m[2][0], m[3][0]}, r1{m[0][1], m[1][1], m[2][1], m[3][1]};
        const glm::vec4 r2{m[0][2], m[1][2], m[2][2], m[3][2]}, r3{m[0][3], m[1][3], m[2][3], m[3][3]};
        planes = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2};
    }
    [[nodiscard]] bool visible(glm::vec3 lo, glm::vec3 hi) const {
        for (const glm::vec4& p : planes) {
            const glm::vec3 far{p.x > 0 ? hi.x : lo.x, p.y > 0 ? hi.y : lo.y, p.z > 0 ? hi.z : lo.z};
            if (p.x * far.x + p.y * far.y + p.z * far.z + p.w < 0.0f) return false;
        }
        return true;
    }
};

struct WorldRenderer {
    std::optional<RendererSystem::GL::Shader> shader;
    std::unordered_map<ChunkKey, GpuMesh, ChunkKeyHash> gpu;
    GpuMesh lines;   ///< Рамка выбранного блока.
    GpuMesh debris;  ///< Осколки: перестраивается каждый кадр.
    std::vector<Vertex> scratch;
    std::uint32_t drawn = 0, culled = 0;
    std::uint64_t uploads = 0;
    std::size_t gpu_bytes = 0;

    void init() {
        auto s = RendererSystem::GL::Shader::from_source(voxel_vs, voxel_fs);
        if (!s) throw std::runtime_error(s.error());
        shader.emplace(std::move(*s));
    }

    /// Синхронизация с Mesher: загрузить новые версии сеток, освободить выгруженные.
    void sync(const Mesher& mesher) {
        for (auto it = gpu.begin(); it != gpu.end();) {
            if (!mesher.meshes.contains(it->first)) {
                it->second.release();
                it = gpu.erase(it);
            } else {
                ++it;
            }
        }
        gpu_bytes = 0;
        for (const auto& [key, mesh] : mesher.meshes) {
            GpuMesh& g = gpu[key];
            if (g.version != mesh.version) {
                g.upload(mesh.vertices);
                g.version = mesh.version;
                ++uploads;
            }
            gpu_bytes += static_cast<std::size_t>(g.count) * sizeof(Vertex);
        }
    }

    void draw(const glm::mat4& view_projection, glm::vec3 eye, glm::vec3 fog, float fog_end, const std::optional<RayHit>& hit,
              std::span<const Vertex> debris_vertices) {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glDisable(GL_BLEND);
        shader->use();
        shader->set("u_view_projection", view_projection);
        shader->set("u_eye", eye);
        shader->set("u_fog_color", fog);
        shader->set("u_fog_range", glm::vec2{fog_end * 0.6f, fog_end});

        const Frustum frustum(view_projection);
        drawn = culled = 0;
        for (const auto& [key, g] : gpu) {
            const glm::vec3 lo{static_cast<float>(key.x * chunk_side), 0.0f, static_cast<float>(key.z * chunk_side)};
            if (!frustum.visible(lo, lo + glm::vec3{chunk_side, chunk_height, chunk_side})) {
                ++culled;
                continue;
            }
            g.draw();
            ++drawn;
        }
        if (!debris_vertices.empty()) {
            debris.upload(debris_vertices, GL_STREAM_DRAW);
            debris.draw();
        }
        if (hit) { // рамка блока под прицелом: 12 рёбер чуть больше блока
            const glm::vec3 lo = glm::vec3(hit->block) - 0.002f, hi = glm::vec3(hit->block) + 1.002f;
            scratch.clear();
            const auto edge = [&](glm::vec3 a, glm::vec3 b) {
                scratch.push_back({a.x, a.y, a.z, 0xFF101010});
                scratch.push_back({b.x, b.y, b.z, 0xFF101010});
            };
            for (int i = 0; i < 4; ++i) {
                const float x0 = (i & 1) ? hi.x : lo.x, z0 = (i & 2) ? hi.z : lo.z;
                edge({x0, lo.y, z0}, {x0, hi.y, z0});
            }
            for (const float y : {lo.y, hi.y}) {
                edge({lo.x, y, lo.z}, {hi.x, y, lo.z}), edge({lo.x, y, hi.z}, {hi.x, y, hi.z});
                edge({lo.x, y, lo.z}, {lo.x, y, hi.z}), edge({hi.x, y, lo.z}, {hi.x, y, hi.z});
            }
            lines.upload(scratch, GL_STREAM_DRAW);
            lines.draw(GL_LINES);
        }
        glBindVertexArray(0);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); // дальше рисует 2D-оверлей Core
    }

    void release() {
        for (auto& [key, g] : gpu) g.release();
        gpu.clear();
        lines.release();
        debris.release();
    }
};

/// Кубик-осколок: 36 вершин в мировых координатах.
void append_cube(std::vector<Vertex>& out, glm::vec3 center, float half, std::uint32_t rgba) {
    static constexpr std::array<std::array<int, 3>, 8> c = {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}};
    static constexpr std::array<int, 36> idx = {0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7, 0, 4, 7, 0, 7, 3,
                                                1, 2, 6, 1, 6, 5, 3, 7, 6, 3, 6, 2, 0, 1, 5, 0, 5, 4};
    for (const int i : idx) {
        const auto& p = c[static_cast<std::size_t>(i)];
        out.push_back({center.x + (p[0] ? half : -half), center.y + (p[1] ? half : -half), center.z + (p[2] ? half : -half), rgba});
    }
}

// =============================================================================
// Игра
// =============================================================================

class Voxel final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {1280.0f, 720.0f}; } // 2D-камера не используется

    void setup(Core::App& app) override {
        const auto& args = app.config().extra_args;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--autopilot") controller.autopilot = interaction.autodig = true;
            if (args[i] == "--radius" && i + 1 < args.size()) terrain.radius = std::clamp(std::atoi(args[i + 1].c_str()), 2, 32);
        }
        es::EventBus& bus = app.bus();
        terrain.declare(bus);
        mesher.declare(bus);
        controller.declare(bus);
        interaction.declare(bus);
        debris_system.declare(bus);
        renderer.init();

        glm::vec3 spawn{8.5f, static_cast<float>(terrain_height(8, 8)) + 1.01f, 8.5f};

        // Стартовый мир — целиком, сразу: замер «сколько чанков в секунду» на всех потоках.
        const auto t0 = std::chrono::steady_clock::now();
        terrain.stream(app.jobs(), spawn, true);
        const auto t1 = std::chrono::steady_clock::now();
        // Сетки строятся по событиям chunk_loaded, а они видны со следующего тика — поэтому здесь напрямую.
        for (const auto& [key, blocks] : terrain.chunks) mesher.dirty.push_back(key);
        mesher.tick(app.jobs(), terrain, spawn, true);
        const auto t2 = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        startup_generate_ms = ms(t0, t1);
        startup_mesh_ms = ms(t1, t2);
        startup_chunks = terrain.chunks.size();

        while (terrain.solid(8, static_cast<int>(spawn.y), 8) || terrain.solid(8, static_cast<int>(spawn.y) + 1, 8)) spawn.y += 1.0f; // не в дереве
        player = world.create();
        world.emplace<Transform>(player, spawn, spawn);
        world.emplace<Velocity>(player);
        world.emplace<PlayerState>(player);
        std::println("Voxel: radius {} chunks -> {} chunks generated in {:.1f} ms, meshed in {:.1f} ms ({} vertices), {} job threads",
                     terrain.radius, terrain.chunks.size(), startup_generate_ms, startup_mesh_ms, mesher.vertices_built,
                     app.jobs().threads());

        capture(app, !controller.autopilot);
    }

    void tick(Core::App& app) override {
        const float dt = app.tick_seconds();
        js::Scheduler& jobs = app.jobs();
        profiler.measure(Profiler::Controller, [&] { controller.tick(world, player, terrain, dt); });
        const glm::vec3 feet = world.get<Transform>(player)->position;
        const glm::vec3 eye = feet + glm::vec3{0.0f, eye_height, 0.0f};
        profiler.measure(Profiler::Interaction, [&] {
            interaction.tick(terrain, eye, look_direction(controller.yaw, controller.pitch), feet, captured, app.tick());
        });
        profiler.measure(Profiler::Terrain, [&] { terrain.tick(jobs, feet); });
        profiler.measure(Profiler::Mesher, [&] { mesher.tick(jobs, terrain, feet); });
        profiler.measure(Profiler::Debris, [&] { debris_system.tick(world, terrain, dt); });
        ++profiler.ticks;
        if (app.tick() > 0 && app.tick() % 600 == 0) {
            std::println("[tick {:>5}] pos ({:.0f}, {:.0f}, {:.0f}) | chunks {} (meshes {}, dirty {}) | drawn {} culled {} | gpu {:.1f} MiB",
                         app.tick(), feet.x, feet.y, feet.z, terrain.chunks.size(), mesher.meshes.size(), mesher.dirty.size(),
                         renderer.drawn, renderer.culled, static_cast<double>(renderer.gpu_bytes) / (1024.0 * 1024.0));
        }
    }

    void render(Core::App& app, Renderer2D& /*r*/) override {
        // Обзор мышью — в домене кадра (плавно при любой частоте тиков). [в модуль: относительный ввод мыши]
        WindowSystem::Window& window = app.window();
        if (window.input().pressed(GLFW_KEY_TAB)) capture(app, !captured);
        if (captured && !controller.autopilot) {
            const WindowSystem::Vec2d d = window.input().cursor_delta();
            controller.yaw += static_cast<float>(d.x) * 0.0025f;
            controller.pitch = std::clamp(controller.pitch - static_cast<float>(d.y) * 0.0025f, -1.55f, 1.55f);
        }

        profiler.measure(Profiler::Upload, [&] { renderer.sync(mesher); });
        profiler.measure(Profiler::Render, [&] {
            const Transform& t = *world.get<Transform>(player);
            const glm::vec3 eye = glm::mix(t.previous, t.position, app.tick_alpha()) + glm::vec3{0.0f, eye_height, 0.0f};
            const glm::vec3 dir = look_direction(controller.yaw, controller.pitch);
            const glm::vec2 viewport = app.camera().viewport;
            const float far = static_cast<float>(terrain.radius * chunk_side);
            const glm::mat4 projection = glm::perspective(glm::radians(70.0f), viewport.x / viewport.y, 0.05f, far + 32.0f);
            const glm::mat4 view = glm::lookAt(eye, eye + dir, glm::vec3{0.0f, 1.0f, 0.0f});

            const glm::vec3 sky{0.55f, 0.74f, 0.95f};
            glClearColor(sky.r, sky.g, sky.b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            debris_vertices.clear();
            world.view<const Transform, const Debris>().each([&](const Transform& dt, const Debris& d) {
                append_cube(debris_vertices, glm::mix(dt.previous, dt.position, app.tick_alpha()), 0.08f, shade(block_color[d.block], 0.9f));
            });
            renderer.draw(projection * view, eye, sky, far, raycast(terrain, eye, dir, 6.0f), debris_vertices);
        });
        ++profiler.frames;
    }

    void render_overlay(Core::App& app, Renderer2D& r) override {
        const glm::vec2 vp = app.camera().viewport;
        const glm::vec2 c = vp * 0.5f;
        r.fill_rect({{c.x - 9.0f, c.y - 1.0f}, {18.0f, 2.0f}}, Colors::white, 5); // прицел
        r.fill_rect({{c.x - 1.0f, c.y - 9.0f}, {2.0f, 18.0f}}, Colors::white, 5);
        const float cell = 40.0f;
        const float x0 = c.x - cell * static_cast<float>(hotbar.size()) * 0.5f;
        for (std::size_t i = 0; i < hotbar.size(); ++i) { // панель блоков
            const glm::vec2 at{x0 + static_cast<float>(i) * cell, vp.y - cell - 12.0f};
            r.fill_rect({at, {cell, cell}}, Color{0, 0, 0, 140}, 3);
            r.fill_rect({at + 6.0f, {cell - 12.0f, cell - 12.0f}}, Color::from_rgba(block_color[hotbar[i]]), 4);
            if (i == interaction.selected) r.draw_rect({at, {cell, cell}}, 3.0f, Colors::white, 5);
        }
        profiler.render_overlay(r, vp);
    }

    [[nodiscard]] std::string status() const override {
        const glm::vec3 p = world.get<Transform>(player)->position;
        return std::format("({:.0f}, {:.0f}, {:.0f}) | {} | chunks {} | drawn {} | block {} | {}", p.x, p.y, p.z,
                           world.get<PlayerState>(player)->flying ? "flying [F]" : "walking [F]", terrain.chunks.size(),
                           renderer.drawn, block_name[hotbar[interaction.selected]], captured ? "Tab — free mouse" : "Tab — capture mouse");
    }

    void shutdown(Core::App& app) override {
        const auto per_tick = [&](Profiler::Section s) { return Profiler::per(profiler.total_ms[s], profiler.ticks); };
        const auto per_frame = [&](Profiler::Section s) { return Profiler::per(profiler.total_ms[s], profiler.frames); };
        std::println("\n===== Voxel : summary, {} ticks, {} frames, radius {} =====", profiler.ticks, profiler.frames, terrain.radius);
        std::println("startup: {} chunks generated in {:.1f} ms, meshed in {:.1f} ms ({:.3f} + {:.3f} ms per chunk, wall time)",
                     startup_chunks, startup_generate_ms, startup_mesh_ms, startup_generate_ms / static_cast<double>(std::max<std::size_t>(startup_chunks, 1)),
                     startup_mesh_ms / static_cast<double>(std::max<std::size_t>(startup_chunks, 1)));
        std::println("streaming: generated {} chunks total, unloaded {}, peak loaded {} | meshes built {} ({} vertices)", terrain.generated,
                     terrain.unloaded, terrain.peak_loaded, mesher.built, mesher.vertices_built);
        std::println("memory: chunk pool {} live x {} KiB = {:.1f} MiB | GPU vertex buffers {:.1f} MiB",
                     terrain.pool.live(), sizeof(ChunkBlocks) / 1024, static_cast<double>(terrain.pool.live() * sizeof(ChunkBlocks)) / (1024.0 * 1024.0),
                     static_cast<double>(renderer.gpu_bytes) / (1024.0 * 1024.0));
        std::println("edits: broken {} placed {} | debris spawned {} | GPU uploads {} | last frame drawn {} culled {}", interaction.broken,
                     interaction.placed, debris_system.spawned, renderer.uploads, renderer.drawn, renderer.culled);
        std::println("\n{:<12} {:>10}", "system", "ms");
        for (std::size_t s = 0; s < Profiler::Upload; ++s) std::println("{:<12} {:>10.3f} / tick", Profiler::names[s], per_tick(static_cast<Profiler::Section>(s)));
        for (std::size_t s = Profiler::Upload; s < Profiler::Count; ++s) std::println("{:<12} {:>10.3f} / frame", Profiler::names[s], per_frame(static_cast<Profiler::Section>(s)));

        // Контрольная сумма загруженного мира (в порядке ключей) и позиции игрока — одинакова при любом --threads.
        std::vector<ChunkKey> keys;
        for (const auto& [key, blocks] : terrain.chunks) keys.push_back(key);
        std::ranges::sort(keys, [](ChunkKey a, ChunkKey b) { return a.packed() < b.packed(); });
        std::uint64_t checksum = 1469598103934665603ULL;
        for (const ChunkKey key : keys) {
            const ChunkBlocks* b = terrain.find(key);
            for (std::size_t i = 0; i < b->blocks.size(); i += 7) checksum = (checksum ^ b->blocks[i]) * 1099511628211ULL;
            const auto it = mesher.meshes.find(key);
            checksum = (checksum ^ (it == mesher.meshes.end() ? 0 : it->second.vertices.size())) * 1099511628211ULL;
        }
        const glm::vec3 p = world.get<Transform>(player)->position;
        for (const float f : {p.x, p.y, p.z}) checksum = (checksum ^ std::bit_cast<std::uint32_t>(f)) * 1099511628211ULL;
        std::println("jobs: {} background threads | world checksum {:016x}", app.jobs().threads(), checksum);
        renderer.release();
    }

private:
    void capture(Core::App& app, bool on) {
        // В WindowSystem нет захвата курсора — напрямую через GLFW. [в модуль: Window::set_cursor_mode]
        captured = on;
        glfwSetInputMode(app.window().native_handle(), GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }

    ECS::World world;
    ECS::Entity player;
    Terrain terrain;
    Mesher mesher;
    Controller controller;
    Interaction interaction;
    DebrisSystem debris_system;
    WorldRenderer renderer;
    Profiler profiler;
    std::vector<Vertex> debris_vertices;
    double startup_generate_ms = 0.0, startup_mesh_ms = 0.0;
    std::size_t startup_chunks = 0;
    bool captured = false;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<Voxel>({.title = "Voxel", .ticks_per_second = 60.0, .pause_key = GLFW_KEY_P}, argc, argv);
}
