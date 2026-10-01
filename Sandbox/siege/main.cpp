/**
 * @file main.cpp
 * @brief Siege — оборона замка: все модули движка в одной игре и замер того, что даёт каждый.
 *
 * Волны врагов (до десятков тысяч одновременно) идут от трёх ворот слева к замку справа
 * по полю потоков (flow field). Строитель ставит башни: лучников (одиночный выстрел) и мортиры
 * (урон по площади). Башни перекрывают проходы — враги перестраивают маршрут, и строитель
 * получает лабиринт.
 *
 * Кто за что отвечает:
 * - **ECSSystem** — враги (Body, Health, Stats) и башни (Tower); ссылки на врагов в событиях —
 *   Entity с поколением: выстрел по уже убитому врагу безопасно отсеивается;
 * - **EventSystem** — модули общаются только событиями (N → N+1). Массовые события —
 *   SoA (`siege.shot`, `siege.impact`, `siege.killed`): десятки тысяч за тик;
 * - **MemorySystem** — пространственная сетка строится каждый тик в памяти тика (ни одного malloc);
 * - **JobSystem** — Navigation, Targeting и Ballistics идут через parallel_for; события кусков
 *   собираются в ChunkBuffers и уходят в шину в порядке кусков. Поэтому игра **одинакова при
 *   любом числе потоков**: `--threads 0` и `--threads 7` дают одну и ту же контрольную сумму;
 * - **WindowSystem / Core / RendererSystem** — окно, ввод, фиксированный тик, отрисовка, оверлей.
 *
 * Порядок тика (модули):
 * 1. Lifecycle   — **единственный** владелец структуры мира: уничтожает убитых и прорвавшихся
 *                  (события прошлого тика), рождает врагов по приказам волн, ставит башни;
 * 2. Waves       — расписание волн: приказы `siege.spawn` на следующий тик;
 * 3. Grid        — сетка врагов в памяти тика (снимок: позиция, здоровье, путь до замка);
 * 4. Navigation  — ∥ поле потоков + разлёт с соседями; дошедшие до замка → `siege.breach`;
 * 5. Targeting   — ∥ каждая башня ищет в сетке врага, ближайшего к замку → `siege.shot`;
 * 6. Ballistics  — снаряды прошлых выстрелов летят ∥, попадания (мортира — по площади) → `siege.impact`;
 * 7. Damage      — урон по порядку событий → `siege.killed`;
 * 8. Economy     — золото, целостность замка, строитель (ИИ и игрок) → `siege.build`.
 *
 * Управление: ЛКМ — лучник, ПКМ — мортира на клетке под курсором; B — автостроитель вкл/выкл;
 * T — параллельные системы ↔ всё в главном потоке; F — показать поле потоков.
 * Аргументы: `--wave N` — начать с волны N (сразу большая нагрузка), `--cap N` — предел врагов
 * (по умолчанию 60 000), `--no-draw` — не рисовать врагов (чистые замеры). Общие — см. Core::App.
 */

#include <Core/Core.hpp>
#include <ECSSystem/ECSSystem.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <format>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace es = EventSystem;
namespace ms = MemorySystem;
namespace js = JobSystem;
using namespace RendererSystem;

namespace {

// =============================================================================
// Карта
// =============================================================================

constexpr int tiles_x = 120;
constexpr int tiles_y = 68;
constexpr float tile = 16.0f;
constexpr float world_w = tiles_x * tile;
constexpr float world_h = tiles_y * tile;
constexpr std::size_t tile_count = static_cast<std::size_t>(tiles_x) * tiles_y;

constexpr float cell = 32.0f; ///< Клетка пространственной сетки врагов (2×2 тайла).
constexpr int cells_x = static_cast<int>(world_w / cell);
constexpr int cells_y = static_cast<int>(world_h / cell);
constexpr std::size_t cell_count = static_cast<std::size_t>(cells_x) * cells_y;

/// Тайл карты. Ноль — свободная земля (ZII: обнулённая карта — пустое поле).
enum class Terrain : std::uint8_t { Free = 0, Rock, Tower, Castle };

constexpr std::array<std::array<int, 2>, 3> gates = {{{1, 10}, {1, 34}, {1, 58}}};
constexpr int castle_x0 = 112, castle_x1 = 117, castle_y0 = 30, castle_y1 = 37;

int tile_index(int x, int y) { return y * tiles_x + x; }
glm::ivec2 tile_of(glm::vec2 p) {
    return {std::clamp(static_cast<int>(p.x / tile), 0, tiles_x - 1), std::clamp(static_cast<int>(p.y / tile), 0, tiles_y - 1)};
}
glm::vec2 tile_center(int x, int y) { return {(static_cast<float>(x) + 0.5f) * tile, (static_cast<float>(y) + 0.5f) * tile}; }
int cell_of(glm::vec2 p) {
    const int cx = std::clamp(static_cast<int>(p.x / cell), 0, cells_x - 1);
    const int cy = std::clamp(static_cast<int>(p.y / cell), 0, cells_y - 1);
    return cy * cells_x + cx;
}
bool blocked(Terrain t) { return t == Terrain::Rock || t == Terrain::Tower; }

/// Стартовая карта: три стены с проходами — враги сходятся в узкие места.
std::vector<Terrain> make_map() {
    std::vector<Terrain> map(tile_count); // Free
    const auto wall = [&](int x, std::initializer_list<std::array<int, 2>> gaps) {
        for (int y = 0; y < tiles_y; ++y) {
            bool gap = false;
            for (const auto& g : gaps) gap = gap || (y >= g[0] && y <= g[1]);
            if (!gap) map[static_cast<std::size_t>(tile_index(x, y))] = map[static_cast<std::size_t>(tile_index(x + 1, y))] = Terrain::Rock;
        }
    };
    wall(30, {{8, 15}, {50, 57}});
    wall(60, {{29, 38}});
    wall(88, {{4, 12}, {55, 63}});
    for (int y = castle_y0; y <= castle_y1; ++y)
        for (int x = castle_x0; x <= castle_x1; ++x) map[static_cast<std::size_t>(tile_index(x, y))] = Terrain::Castle;
    return map;
}

/// Поле потоков: расстояние до замка (BFS) и направление «вниз по склону» для каждого тайла.
/// Чистые данные: каждый модуль, которому нужна карта, держит свою копию и обновляет её по `siege.build`.
struct FlowField {
    static constexpr std::uint16_t unreachable = 0xFFFF;
    std::vector<std::uint16_t> dist = std::vector<std::uint16_t>(tile_count, unreachable);
    std::vector<glm::vec2> dir = std::vector<glm::vec2>(tile_count);

    void build(const std::vector<Terrain>& map) {
        std::ranges::fill(dist, unreachable);
        std::vector<int> queue;
        queue.reserve(tile_count);
        for (int i = 0; i < static_cast<int>(tile_count); ++i) {
            if (map[static_cast<std::size_t>(i)] == Terrain::Castle) {
                dist[static_cast<std::size_t>(i)] = 0;
                queue.push_back(i);
            }
        }
        constexpr std::array<std::array<int, 2>, 4> steps = {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
        for (std::size_t head = 0; head < queue.size(); ++head) {
            const int x = queue[head] % tiles_x, y = queue[head] / tiles_x;
            for (const auto& s : steps) {
                const int nx = x + s[0], ny = y + s[1];
                if (nx < 0 || ny < 0 || nx >= tiles_x || ny >= tiles_y) continue;
                const auto n = static_cast<std::size_t>(tile_index(nx, ny));
                if (blocked(map[n]) || dist[n] != unreachable) continue;
                dist[n] = static_cast<std::uint16_t>(dist[static_cast<std::size_t>(queue[head])] + 1);
                queue.push_back(static_cast<int>(n));
            }
        }
        // Направление — к соседу (из 8) с наименьшим расстоянием; диагональ — только если не режет угол.
        for (int y = 0; y < tiles_y; ++y) {
            for (int x = 0; x < tiles_x; ++x) {
                const auto i = static_cast<std::size_t>(tile_index(x, y));
                dir[i] = {};
                if (dist[i] == unreachable || dist[i] == 0) continue;
                std::uint16_t best = dist[i];
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= tiles_x || ny >= tiles_y) continue;
                        if (dx != 0 && dy != 0 &&
                            (blocked(map[static_cast<std::size_t>(tile_index(x + dx, y))]) ||
                             blocked(map[static_cast<std::size_t>(tile_index(x, y + dy))])))
                            continue;
                        const std::uint16_t d = dist[static_cast<std::size_t>(tile_index(nx, ny))];
                        if (d < best) {
                            best = d;
                            dir[i] = glm::normalize(glm::vec2{static_cast<float>(dx), static_cast<float>(dy)});
                        }
                    }
                }
            }
        }
    }

    [[nodiscard]] bool gates_reachable() const {
        for (const auto& g : gates)
            if (dist[static_cast<std::size_t>(tile_index(g[0], g[1]))] == unreachable) return false;
        return true;
    }
};

// =============================================================================
// Компоненты
// =============================================================================

/// Горячие данные врага: позиция и скорость (16 байт, плотно в пуле).
struct Body {
    glm::vec2 position{0.0f};
    glm::vec2 velocity{0.0f};
};
/// Здоровье врага. hp ≤ 0 — убит, ждёт уничтожения в Lifecycle.
struct Health {
    float hp = 0.0f;
    float max_hp = 0.0f;
};
/// Холодные данные врага.
struct Stats {
    float speed = 0.0f;
    std::uint32_t reward = 0;
};

enum class TowerKind : std::uint32_t { Archer = 0, Mortar = 1 };

struct TowerSpec {
    std::uint32_t cost;
    float range;
    int cooldown; ///< Тиков между выстрелами.
    float damage;
    float splash; ///< Радиус урона по площади; 0 — одиночная цель.
    float projectile_speed;
};
constexpr std::array<TowerSpec, 2> tower_specs = {{
    {50, 6.0f * tile, 9, 6.0f, 0.0f, 520.0f},
    {140, 9.0f * tile, 40, 7.0f, 34.0f, 260.0f},
}};

/// Башня. cooldown — тиков до следующего выстрела; пишет только Targeting (своя строка).
struct Tower {
    glm::vec2 position{0.0f};
    TowerKind kind = TowerKind::Archer;
    int cooldown = 0;
    std::uint32_t shots = 0;
};

// =============================================================================
// События
// =============================================================================

ECS::Entity entity(std::uint32_t index, std::uint32_t generation) { return ECS::Entity{index, generation}; }

/// Приказ волны: родить врагов у ворот. AoS — несколько штук за тик.
struct SpawnEvent {
    std::uint32_t gate = 0;
    std::uint32_t count = 0;
    std::uint32_t wave = 0;
    float hp = 0.0f;
    float speed = 0.0f;

    static constexpr std::string_view event_name = "siege.spawn";
    using fields = es::Fields<es::Field<"gate", &SpawnEvent::gate>, es::Field<"count", &SpawnEvent::count>,
                              es::Field<"wave", &SpawnEvent::wave>, es::Field<"hp", &SpawnEvent::hp>,
                              es::Field<"speed", &SpawnEvent::speed>>;
};

/// Башня выстрелила. SoA — тысячи за тик.
struct ShotEvent {
    float from_x = 0.0f;
    float from_y = 0.0f;
    std::uint32_t target_index = 0;
    std::uint32_t target_generation = 0;
    std::uint32_t kind = 0;

    static constexpr std::string_view event_name = "siege.shot";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<es::Field<"from_x", &ShotEvent::from_x>, es::Field<"from_y", &ShotEvent::from_y>,
                              es::Field<"target_index", &ShotEvent::target_index>,
                              es::Field<"target_generation", &ShotEvent::target_generation>,
                              es::Field<"kind", &ShotEvent::kind>>;
};

/// Снаряд попал: урон одному врагу (мортира даёт по событию на каждого в радиусе). SoA.
struct ImpactEvent {
    std::uint32_t target_index = 0;
    std::uint32_t target_generation = 0;
    float damage = 0.0f;

    static constexpr std::string_view event_name = "siege.impact";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<es::Field<"target_index", &ImpactEvent::target_index>,
                              es::Field<"target_generation", &ImpactEvent::target_generation>,
                              es::Field<"damage", &ImpactEvent::damage>>;
};

/// Враг убит. SoA.
struct KilledEvent {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    std::uint32_t reward = 0;
    float x = 0.0f;
    float y = 0.0f;

    static constexpr std::string_view event_name = "siege.killed";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<es::Field<"index", &KilledEvent::index>, es::Field<"generation", &KilledEvent::generation>,
                              es::Field<"reward", &KilledEvent::reward>, es::Field<"x", &KilledEvent::x>,
                              es::Field<"y", &KilledEvent::y>>;
};

/// Враг дошёл до замка.
struct BreachEvent {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    static constexpr std::string_view event_name = "siege.breach";
    using fields = es::Fields<es::Field<"index", &BreachEvent::index>, es::Field<"generation", &BreachEvent::generation>>;
};

/// Поставить башню на тайл (уже проверено: путь к замку не перекрыт).
struct BuildEvent {
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    std::uint32_t kind = 0;

    static constexpr std::string_view event_name = "siege.build";
    using fields = es::Fields<es::Field<"tile_x", &BuildEvent::tile_x>, es::Field<"tile_y", &BuildEvent::tile_y>,
                              es::Field<"kind", &BuildEvent::kind>>;
};

constexpr es::ChannelConfig mass_channel{.reserve = 4096, .max_events_per_tick = 1u << 20};

// =============================================================================
// Profiler
// =============================================================================

struct Profiler {
    enum Section : std::size_t { Lifecycle, Grid, Navigation, Targeting, Ballistics, Damage, Economy, Render, Count };
    static constexpr std::array<std::string_view, Count> names = {"lifecycle", "grid",   "navigation", "targeting",
                                                                  "ballistics", "damage", "economy",    "render"};
    static constexpr std::array<bool, Count> parallel = {false, true, true, true, true, false, true, false};
    using Clock = std::chrono::steady_clock;

    std::array<double, Count> total_ms{};
    std::array<double, Count> last_ms{};
    std::uint64_t ticks = 0;
    std::uint64_t frames = 0;

    template<typename Fn>
    void measure(Section s, Fn&& fn) {
        const auto start = Clock::now();
        fn();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        total_ms[s] += ms;
        last_ms[s] = ms;
    }
    [[nodiscard]] static double per(double sum, std::uint64_t n) { return n > 0 ? sum / static_cast<double>(n) : 0.0; }
    [[nodiscard]] double last_tick_ms() const {
        double sum = 0.0;
        for (std::size_t s = 0; s < Render; ++s) sum += last_ms[s];
        return sum;
    }

    /// Столбики: высота — миллисекунды последнего замера (1 мс = 10 px), параллельные системы — с рамкой.
    void render_overlay(Renderer2D& r, glm::vec2 viewport) const {
        static constexpr std::array<std::uint32_t, Count> colors = {0xBA68C8FF, 0x4FC3F7FF, 0x81C784FF, 0xFFB74DFF,
                                                                    0xE57373FF, 0xF06292FF, 0xFFD54FFF, 0xFFF176FF};
        const glm::vec2 origin{viewport.x - 12.0f - static_cast<float>(Count) * 18.0f, viewport.y - 12.0f};
        r.fill_rect({{origin.x - 6.0f, origin.y - 206.0f}, {static_cast<float>(Count) * 18.0f + 12.0f, 212.0f}},
                    Color{0, 0, 0, 170}, 0);
        for (int ms = 2; ms <= 20; ms += 2) {
            r.fill_rect({{origin.x - 6.0f, origin.y - static_cast<float>(ms) * 10.0f}, {4.0f, 1.0f}}, Colors::white, 1);
        }
        for (std::size_t s = 0; s < Count; ++s) {
            const float h = std::max(std::min(static_cast<float>(last_ms[s]) * 10.0f, 200.0f), 1.0f);
            const Rect bar{{origin.x + static_cast<float>(s) * 18.0f, origin.y - h}, {12.0f, h}};
            r.fill_rect(bar, Color::from_rgba(colors[s]), 2);
            if (parallel[s]) r.draw_rect(bar, 1.0f, Colors::white, 3);
        }
    }
};

// =============================================================================
// Пространственная сетка (память тика)
// =============================================================================

/// Снимок врагов, отсортированный по клеткам. Строится каждый тик в арене тика и читается
/// параллельными системами (только чтение) — гонок нет по построению.
struct EnemyGrid {
    std::span<std::uint32_t> cell_start;
    std::span<glm::vec2> position;
    std::span<std::uint32_t> row;      ///< Строка в пуле Body.
    std::span<float> hp;
    std::span<std::uint16_t> progress; ///< Шагов до замка: башни бьют того, кто ближе к прорыву.

    void build(js::Scheduler& jobs, ms::Arena& arena, const ECS::World& world, const FlowField& flow) {
        const ECS::ComponentPool<Body>& bodies = *world.find_pool<Body>();
        const std::span<const Body> b = bodies.components();
        const std::span<const ECS::Entity> entities = bodies.entities();
        const std::size_t n = b.size();
        cell_start = arena.push_array<std::uint32_t>(cell_count + 1); // нули — счётчики готовы (ZII)
        position = arena.push_array<glm::vec2>(n);
        row = arena.push_array<std::uint32_t>(n);
        hp = arena.push_array<float>(n);
        progress = arena.push_array<std::uint16_t>(n);
        const std::span<std::uint32_t> cell_of_row = arena.push_array<std::uint32_t>(n);

        // ∥ клетка каждого врага: каждый кусок пишет только свои элементы.
        js::parallel_for(jobs, n, 8192, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) cell_of_row[i] = static_cast<std::uint32_t>(cell_of(b[i].position));
        });
        for (std::size_t i = 0; i < n; ++i) ++cell_start[cell_of_row[i] + 1];
        for (std::size_t c = 0; c < cell_count; ++c) cell_start[c + 1] += cell_start[c];
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t slot = cell_start[cell_of_row[i]]++;
            row[slot] = static_cast<std::uint32_t>(i);
        }
        for (std::size_t c = cell_count; c > 0; --c) cell_start[c] = cell_start[c - 1];
        cell_start[0] = 0;
        // ∥ копия данных в порядке клеток (здоровье — случайный доступ к другому пулу, его и распараллеливаем).
        js::parallel_for(jobs, n, 4096, [&](std::size_t begin, std::size_t end) {
            for (std::size_t k = begin; k < end; ++k) {
                const Body& body = b[row[k]];
                position[k] = body.position;
                const Health* h = world.get<Health>(entities[row[k]]);
                hp[k] = h ? h->hp : 0.0f;
                const glm::ivec2 t = tile_of(body.position);
                progress[k] = flow.dist[static_cast<std::size_t>(tile_index(t.x, t.y))];
            }
        });
    }

    template<typename Fn>
    void for_each_in(glm::vec2 center, float radius, Fn&& fn) const {
        const int x0 = std::max(static_cast<int>((center.x - radius) / cell), 0);
        const int x1 = std::min(static_cast<int>((center.x + radius) / cell), cells_x - 1);
        const int y0 = std::max(static_cast<int>((center.y - radius) / cell), 0);
        const int y1 = std::min(static_cast<int>((center.y + radius) / cell), cells_y - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const auto c = static_cast<std::size_t>(y * cells_x + x);
                for (std::uint32_t k = cell_start[c]; k < cell_start[c + 1]; ++k) {
                    if (!fn(k)) return;
                }
            }
        }
    }
};

// =============================================================================
// Модули
// =============================================================================

/// Lifecycle — единственный, кто создаёт и уничтожает сущности. Идёт первым в тике.
struct Lifecycle {
    es::EventReader<SpawnEvent> spawns;
    es::EventReader<KilledEvent> killed;
    es::EventReader<BreachEvent> breaches;
    es::EventReader<BuildEvent> builds;
    std::size_t cap = 60'000;
    std::uint64_t spawned = 0;
    std::uint64_t skipped = 0;     ///< Не рождены из-за предела врагов.
    std::size_t peak_alive = 0;
    std::uint32_t archers = 0;
    std::uint32_t mortars = 0;
    std::mt19937 rng{1337};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Lifecycle")
                                    .consumes<SpawnEvent>()
                                    .consumes<KilledEvent>()
                                    .consumes<BreachEvent>()
                                    .consumes<BuildEvent>();
        spawns = bus.reader<SpawnEvent>(id);
        killed = bus.reader<KilledEvent>(id);
        breaches = bus.reader<BreachEvent>(id);
        builds = bus.reader<BuildEvent>(id);
    }

    void tick(ECS::World& world) {
        const auto index = killed.column<&KilledEvent::index>();
        const auto generation = killed.column<&KilledEvent::generation>();
        for (std::size_t k = 0; k < index.size(); ++k) world.destroy(entity(index[k], generation[k]));
        for (const BreachEvent& b : breaches.events()) world.destroy(entity(b.index, b.generation));

        for (const BuildEvent& b : builds.events()) {
            const ECS::Entity e = world.create();
            const auto kind = static_cast<TowerKind>(b.kind);
            world.emplace<Tower>(e, tile_center(b.tile_x, b.tile_y), kind, 0, 0u);
            (kind == TowerKind::Archer ? archers : mortars) += 1;
        }

        std::uniform_real_distribution<float> jitter(-tile * 1.5f, tile * 1.5f);
        for (const SpawnEvent& s : spawns.events()) {
            const glm::vec2 at = tile_center(gates[s.gate][0], gates[s.gate][1]);
            for (std::uint32_t i = 0; i < s.count; ++i) {
                if (world.count<Body>() >= cap) {
                    ++skipped;
                    continue;
                }
                const ECS::Entity e = world.create();
                world.emplace<Body>(e, glm::vec2{std::max(at.x + jitter(rng), 1.0f), at.y + jitter(rng)}, glm::vec2{0.0f});
                world.emplace<Health>(e, s.hp, s.hp);
                world.emplace<Stats>(e, s.speed, 2u + s.wave / 3u);
                ++spawned;
            }
        }
        peak_alive = std::max(peak_alive, world.count<Body>());
    }
};

/// Waves — расписание: волна каждые 15 секунд, численность растёт в 1.4 раза, здоровье — в 1.12.
struct Waves {
    static constexpr es::Tick period = 450;   ///< Тиков между началами волн (15 с при 30 Гц).
    static constexpr es::Tick duration = 300; ///< Волна выходит из ворот за 10 с.
    es::EventWriter<SpawnEvent> out;
    std::uint32_t first_wave = 0;
    std::uint32_t wave = 0;
    std::array<double, gates.size()> owed{}; ///< Дробная часть «сколько ещё родить» у каждых ворот.

    void declare(es::EventBus& bus) {
        out = bus.writer<SpawnEvent>(bus.declare_module("Waves").produces<SpawnEvent>(es::ChannelConfig{.reserve = 16, .max_events_per_tick = 64}));
    }

    [[nodiscard]] static double wave_size(std::uint32_t w) { return 120.0 * std::pow(1.4, w); }

    void tick(es::Tick now) {
        const es::Tick local = now % period;
        wave = first_wave + static_cast<std::uint32_t>(now / period);
        if (local >= duration) return;
        const double per_gate_tick = wave_size(wave) / static_cast<double>(duration * gates.size());
        const auto hp = static_cast<float>(8.0 * std::pow(1.12, wave));
        const float speed = 34.0f + static_cast<float>(wave % 3) * 8.0f;
        for (std::uint32_t g = 0; g < gates.size(); ++g) {
            owed[g] += per_gate_tick;
            const auto count = static_cast<std::uint32_t>(owed[g]);
            if (count == 0) continue;
            owed[g] -= count;
            out.emit(SpawnEvent{.gate = g, .count = count, .wave = wave, .hp = hp, .speed = speed});
        }
    }
};

/// Navigation — ∥ движение по полю потоков с разлётом. Каждый враг пишет только свою строку Body.
struct Navigation {
    es::EventReader<BuildEvent> builds;
    es::EventWriter<BreachEvent> breach_out;
    std::vector<Terrain> map = make_map();
    FlowField flow;
    js::ChunkBuffers<BreachEvent> breach_chunks;
    std::uint32_t rebuilds = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Navigation").consumes<BuildEvent>().produces<BreachEvent>(mass_channel);
        builds = bus.reader<BuildEvent>(id);
        breach_out = bus.writer<BreachEvent>(id);
        flow.build(map);
    }

    void tick(js::Scheduler& jobs, ECS::World& world, const EnemyGrid& grid, float dt) {
        if (!builds.empty()) { // башня перекрыла тайл — поле потоков пересчитывается (BFS, ~0.1 мс)
            for (const BuildEvent& b : builds.events()) map[static_cast<std::size_t>(tile_index(b.tile_x, b.tile_y))] = Terrain::Tower;
            flow.build(map);
            ++rebuilds;
        }
        ECS::ComponentPool<Body>& bodies = world.pool<Body>();
        const std::span<Body> b = bodies.components();
        const std::span<const ECS::Entity> entities = bodies.entities();
        const ECS::World& view = world; // внутри кусков — только чтение мира

        constexpr std::size_t grain = 1024;
        breach_chunks.reset(js::chunk_count(grid.position.size(), grain));
        js::parallel_for(jobs, grid.position.size(), grain, [&](std::size_t begin, std::size_t end, std::size_t chunk) {
            for (std::size_t k = begin; k < end; ++k) {
                if (grid.hp[k] <= 0.0f) continue; // убит, ждёт уничтожения
                Body& body = b[grid.row[k]];
                const ECS::Entity e = entities[grid.row[k]];
                const float speed = view.get<Stats>(e)->speed;
                const glm::vec2 p = grid.position[k];
                const glm::ivec2 t = tile_of(p);
                const auto ti = static_cast<std::size_t>(tile_index(t.x, t.y));

                glm::vec2 desired = flow.dir[ti] * speed;
                if (blocked(map[ti])) { // башню поставили прямо под ногами — выталкиваемся из тайла
                    const glm::vec2 out = p - tile_center(t.x, t.y);
                    desired = (glm::dot(out, out) > 1e-4f ? glm::normalize(out) : glm::vec2{-1.0f, 0.0f}) * speed;
                }
                glm::vec2 separation{0.0f};
                int examined = 0;
                grid.for_each_in(p, 6.0f, [&](std::uint32_t j) {
                    if (j == k) return true;
                    const glm::vec2 d = p - grid.position[j];
                    const float dist2 = d.x * d.x + d.y * d.y;
                    if (dist2 < 36.0f && dist2 > 1e-4f) separation += d / dist2;
                    return ++examined < 12;
                });
                glm::vec2 v = body.velocity + (desired - body.velocity) * 0.25f + separation * 60.0f;
                const float len = std::sqrt(v.x * v.x + v.y * v.y);
                if (len > speed * 1.3f) v *= speed * 1.3f / len;

                // Шаг с простым столкновением со стенами: по осям отдельно.
                glm::vec2 next = p;
                const glm::vec2 step = v * dt;
                if (const glm::ivec2 tx = tile_of({p.x + step.x, p.y}); !blocked(map[static_cast<std::size_t>(tile_index(tx.x, tx.y))]) || blocked(map[ti]))
                    next.x = std::clamp(p.x + step.x, 0.5f, world_w - 0.5f);
                if (const glm::ivec2 ty = tile_of({next.x, p.y + step.y}); !blocked(map[static_cast<std::size_t>(tile_index(ty.x, ty.y))]) || blocked(map[ti]))
                    next.y = std::clamp(p.y + step.y, 0.5f, world_h - 0.5f);
                body.position = next;
                body.velocity = v;

                const glm::ivec2 nt = tile_of(next);
                if (map[static_cast<std::size_t>(tile_index(nt.x, nt.y))] == Terrain::Castle) {
                    breach_chunks[chunk].push_back(BreachEvent{.index = e.index, .generation = e.generation});
                }
            }
        });
        breach_chunks.for_each([&](const BreachEvent& e) { breach_out.emit(e); }); // порядок кусков
    }
};

/// Targeting — ∥ по башням: у каждой своя строка (cooldown), враги — только чтение из сетки.
struct Targeting {
    es::EventWriter<ShotEvent> shots;
    js::ChunkBuffers<ShotEvent> chunks;
    std::uint64_t fired = 0;

    void declare(es::EventBus& bus) {
        shots = bus.writer<ShotEvent>(bus.declare_module("Targeting").produces<ShotEvent>(mass_channel));
    }

    void tick(js::Scheduler& jobs, ECS::World& world, const EnemyGrid& grid) {
        ECS::ComponentPool<Tower>* pool = world.find_pool<Tower>();
        if (!pool) return;
        const std::span<Tower> towers = pool->components();
        const std::span<const ECS::Entity> enemy_entities = world.pool<Body>().entities();
        constexpr std::size_t grain = 32; // башня — сотни кандидатов: кусок «весомый» и при малом grain
        chunks.reset(js::chunk_count(towers.size(), grain));
        js::parallel_for(jobs, towers.size(), grain, [&](std::size_t begin, std::size_t end, std::size_t chunk) {
            for (std::size_t i = begin; i < end; ++i) {
                Tower& tower = towers[i];
                if (tower.cooldown > 0) {
                    --tower.cooldown;
                    continue;
                }
                const TowerSpec& spec = tower_specs[static_cast<std::size_t>(tower.kind)];
                std::uint32_t best = UINT32_MAX;
                std::uint16_t best_progress = FlowField::unreachable;
                int examined = 0;
                grid.for_each_in(tower.position, spec.range, [&](std::uint32_t k) {
                    if (grid.hp[k] <= 0.0f) return true;
                    const glm::vec2 d = grid.position[k] - tower.position;
                    if (d.x * d.x + d.y * d.y > spec.range * spec.range) return true;
                    if (grid.progress[k] < best_progress) { // ближе всех к замку; при равенстве — первый в сетке
                        best_progress = grid.progress[k];
                        best = k;
                    }
                    return ++examined < 256;
                });
                if (best == UINT32_MAX) continue;
                const ECS::Entity target = enemy_entities[grid.row[best]];
                chunks[chunk].push_back(ShotEvent{.from_x = tower.position.x, .from_y = tower.position.y,
                                                  .target_index = target.index, .target_generation = target.generation,
                                                  .kind = static_cast<std::uint32_t>(tower.kind)});
                tower.cooldown = spec.cooldown;
                ++tower.shots;
            }
        });
        fired += chunks.total();
        chunks.for_each([&](const ShotEvent& s) { shots.emit(s); });
    }
};

/// Ballistics — снаряды летят к цели (самонаведение), попадание → `siege.impact`. Мортира бьёт по площади.
struct Ballistics {
    struct Projectile {
        glm::vec2 position{0.0f};
        glm::vec2 aim{0.0f};       ///< Последняя известная позиция цели.
        ECS::Entity target{};
        TowerKind kind = TowerKind::Archer;
        bool done = false;
    };

    es::EventReader<ShotEvent> shots;
    es::EventWriter<ImpactEvent> impacts;
    std::vector<Projectile> flying;
    js::ChunkBuffers<ImpactEvent> chunks;
    std::uint64_t impacts_total = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Ballistics").consumes<ShotEvent>().produces<ImpactEvent>(mass_channel);
        shots = bus.reader<ShotEvent>(id);
        impacts = bus.writer<ImpactEvent>(id);
    }

    void tick(js::Scheduler& jobs, const ECS::World& world, const EnemyGrid& grid, float dt) {
        // Новые снаряды — из SoA-колонок выстрелов прошлого тика, в порядке событий.
        const auto fx = shots.column<&ShotEvent::from_x>();
        const auto fy = shots.column<&ShotEvent::from_y>();
        const auto ti = shots.column<&ShotEvent::target_index>();
        const auto tg = shots.column<&ShotEvent::target_generation>();
        const auto kind = shots.column<&ShotEvent::kind>();
        for (std::size_t s = 0; s < fx.size(); ++s) {
            flying.push_back(Projectile{.position = {fx[s], fy[s]}, .aim = {fx[s], fy[s]}, .target = entity(ti[s], tg[s]),
                                        .kind = static_cast<TowerKind>(kind[s])});
        }

        const std::span<const ECS::Entity> enemy_entities = world.find_pool<Body>()->entities();
        constexpr std::size_t grain = 512;
        chunks.reset(js::chunk_count(flying.size(), grain));
        js::parallel_for(jobs, flying.size(), grain, [&](std::size_t begin, std::size_t end, std::size_t chunk) {
            for (std::size_t i = begin; i < end; ++i) {
                Projectile& p = flying[i];
                const TowerSpec& spec = tower_specs[static_cast<std::size_t>(p.kind)];
                if (const Body* body = world.get<Body>(p.target)) p.aim = body->position; // жив — доворачиваем
                const glm::vec2 to = p.aim - p.position;
                const float dist = std::sqrt(to.x * to.x + to.y * to.y);
                const float step = spec.projectile_speed * dt;
                if (dist > step + 3.0f) {
                    p.position += to / dist * step;
                    continue;
                }
                p.position = p.aim;
                p.done = true;
                if (spec.splash <= 0.0f) {
                    if (world.valid(p.target))
                        chunks[chunk].push_back({.target_index = p.target.index, .target_generation = p.target.generation, .damage = spec.damage});
                } else {
                    grid.for_each_in(p.aim, spec.splash, [&](std::uint32_t k) {
                        const glm::vec2 d = grid.position[k] - p.aim;
                        if (grid.hp[k] > 0.0f && d.x * d.x + d.y * d.y <= spec.splash * spec.splash) {
                            const ECS::Entity victim = enemy_entities[grid.row[k]];
                            chunks[chunk].push_back({.target_index = victim.index, .target_generation = victim.generation, .damage = spec.damage});
                        }
                        return true;
                    });
                }
            }
        });
        impacts_total += chunks.total();
        chunks.for_each([&](const ImpactEvent& e) { impacts.emit(e); });
        std::erase_if(flying, [](const Projectile& p) { return p.done; }); // устойчиво: порядок сохраняется
    }
};

/// Damage — урон по порядку событий (порядок важен: кто добил — тот и убил). Пишет только Health.
struct Damage {
    es::EventReader<ImpactEvent> impacts;
    es::EventWriter<KilledEvent> killed_out;
    std::uint64_t kills = 0;
    std::uint64_t wasted = 0; ///< Попадания по уже убитым / уничтоженным.

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Damage").consumes<ImpactEvent>().produces<KilledEvent>(mass_channel);
        impacts = bus.reader<ImpactEvent>(id);
        killed_out = bus.writer<KilledEvent>(id);
    }

    void tick(ECS::World& world) {
        const auto index = impacts.column<&ImpactEvent::target_index>();
        const auto generation = impacts.column<&ImpactEvent::target_generation>();
        const auto damage = impacts.column<&ImpactEvent::damage>();
        for (std::size_t k = 0; k < index.size(); ++k) {
            const ECS::Entity e = entity(index[k], generation[k]);
            Health* h = world.get<Health>(e);
            if (!h || h->hp <= 0.0f) {
                ++wasted;
                continue;
            }
            h->hp -= damage[k];
            if (h->hp <= 0.0f) {
                const Body* body = world.get<Body>(e);
                killed_out.emit(KilledEvent{.index = e.index, .generation = e.generation,
                                            .reward = world.get<Stats>(e)->reward, .x = body->position.x, .y = body->position.y});
                ++kills;
            }
        }
    }
};

/// Economy — золото, целостность замка и строитель. Держит свою копию карты, чтобы проверять,
/// что новая башня не перекроет путь (BFS на копии) — модулю не нужен доступ к чужим данным.
struct Economy {
    es::EventReader<KilledEvent> killed;
    es::EventReader<BreachEvent> breaches;
    es::EventReader<Core::MouseButtonEvent> mouse;
    es::EventReader<Core::KeyEvent> keys;
    es::EventWriter<BuildEvent> build_out;

    std::vector<Terrain> map = make_map();
    FlowField flow;
    std::vector<std::uint8_t> on_path = std::vector<std::uint8_t>(tile_count);
    std::vector<std::uint8_t> coverage = std::vector<std::uint8_t>(tile_count); ///< Сколько башен достаёт до тайла.
    std::uint64_t gold = 500;
    std::uint64_t earned = 0;
    std::int64_t integrity = 500;
    std::uint64_t breached = 0;
    std::uint32_t built = 0;
    std::uint32_t player_built = 0;
    bool auto_build = true;
    es::Tick fallen_at = 0;

    void declare(es::EventBus& bus, es::ModuleId /*platform*/) {
        const es::ModuleId id = bus.declare_module("Economy")
                                    .consumes<KilledEvent>()
                                    .consumes<BreachEvent>()
                                    .consumes<Core::MouseButtonEvent>()
                                    .consumes<Core::KeyEvent>()
                                    .produces<BuildEvent>(es::ChannelConfig{.reserve = 16, .max_events_per_tick = 256});
        killed = bus.reader<KilledEvent>(id);
        breaches = bus.reader<BreachEvent>(id);
        mouse = bus.reader<Core::MouseButtonEvent>(id);
        keys = bus.reader<Core::KeyEvent>(id);
        build_out = bus.writer<BuildEvent>(id);
        refresh_path();
    }

    [[nodiscard]] std::uint64_t cost(TowerKind kind) const {
        return tower_specs[static_cast<std::size_t>(kind)].cost + built * 2u; // каждая башня дорожает следующую
    }

    void tick(js::Scheduler& jobs, es::Tick now) {
        for (const std::uint32_t reward : killed.column<&KilledEvent::reward>()) {
            gold += reward;
            earned += reward;
        }
        breached += breaches.size();
        integrity -= static_cast<std::int64_t>(breaches.size());
        if (integrity <= 0 && fallen_at == 0) {
            fallen_at = now;
            std::println("[tick {}] the castle has fallen — the siege continues in endless mode", now);
        }

        for (const Core::KeyEvent& key : keys.events())
            if (key.action == GLFW_PRESS && key.key == GLFW_KEY_B) auto_build = !auto_build;
        for (const Core::MouseButtonEvent& m : mouse.events()) {
            if (m.action != GLFW_PRESS || m.button > GLFW_MOUSE_BUTTON_RIGHT) continue;
            const glm::ivec2 t = tile_of({m.world_x, m.world_y});
            const TowerKind kind = m.button == GLFW_MOUSE_BUTTON_LEFT ? TowerKind::Archer : TowerKind::Mortar;
            if (try_build(t.x, t.y, kind)) ++player_built;
        }
        if (auto_build && now % 5 == 0) {
            for (int i = 0; i < 4 && auto_builder(jobs); ++i) {} // до четырёх башен за решение, пока хватает золота
        }
    }

    struct Candidate {
        int score = 0; ///< Ноль — «кандидата нет» (ZII).
        int tile = 0;
        /// Строгий порядок: больше очков, при равенстве — меньший номер тайла. От потоков не зависит.
        [[nodiscard]] bool better(const Candidate& o) const { return score != o.score ? score > o.score : tile < o.tile; }
    };
    using TopCandidates = std::array<Candidate, 6>;

    static TopCandidates merge(TopCandidates a, const TopCandidates& b) {
        for (const Candidate& c : b) {
            if (c.score == 0 || !c.better(a.back())) continue;
            a.back() = c;
            std::ranges::sort(a, [](const Candidate& l, const Candidate& r) { return l.better(r); });
        }
        return a;
    }

    /// Строитель: клетка, из которой башня накроет больше всего тайлов текущего маршрута.
    /// Каждая четвёртая — мортира. Оценка всех клеток — parallel_reduce по строкам карты:
    /// лучшие кандидаты кусков сливаются в порядке кусков, порядок кандидатов строгий — выбор
    /// одинаков при любом числе потоков.
    bool auto_builder(js::Scheduler& jobs) {
        const TowerKind kind = built % 4 == 3 ? TowerKind::Mortar : TowerKind::Archer;
        if (gold < cost(kind)) return false;
        const int reach = static_cast<int>(tower_specs[static_cast<std::size_t>(kind)].range / tile);
        const TopCandidates best = js::parallel_reduce(
            jobs, static_cast<std::size_t>(tiles_y - 2), 4, TopCandidates{},
            [&](std::size_t begin, std::size_t end) {
                TopCandidates top{};
                for (int y = static_cast<int>(begin) + 1; y < static_cast<int>(end) + 1; ++y) {
                    for (int x = 6; x < castle_x0 - 2; ++x) {
                        const int i = tile_index(x, y);
                        if (map[static_cast<std::size_t>(i)] != Terrain::Free) continue;
                        int score = 0;
                        for (int dy = -reach; dy <= reach; ++dy)
                            for (int dx = -reach; dx <= reach; ++dx) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= tiles_x || ny >= tiles_y || dx * dx + dy * dy > reach * reach) continue;
                                const auto n = static_cast<std::size_t>(tile_index(nx, ny));
                                // Убывающая отдача: каждая башня, уже достающая до тайла, вдвое снижает его вес.
                                score += on_path[n] * (8 >> std::min<int>(coverage[n], 3));
                            }
                        top = merge(top, TopCandidates{Candidate{score, i}});
                    }
                }
                return top;
            },
            merge);
        for (const Candidate& c : best) {
            if (c.score > 0 && try_build(c.tile % tiles_x, c.tile / tiles_x, kind)) return true;
        }
        return false;
    }

    bool try_build(int x, int y, TowerKind kind) {
        if (x < 3 || x >= castle_x0 - 1 || y < 0 || y >= tiles_y) return false;
        const auto i = static_cast<std::size_t>(tile_index(x, y));
        if (map[i] != Terrain::Free || gold < cost(kind)) return false;
        map[i] = Terrain::Tower;
        flow.build(map);
        if (!flow.gates_reachable()) { // перекрыла бы путь — отменяем
            map[i] = Terrain::Free;
            flow.build(map);
            return false;
        }
        gold -= cost(kind);
        ++built;
        const int reach = static_cast<int>(tower_specs[static_cast<std::size_t>(kind)].range / tile);
        for (int dy = -reach; dy <= reach; ++dy)
            for (int dx = -reach; dx <= reach; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < tiles_x && ny < tiles_y && dx * dx + dy * dy <= reach * reach)
                    coverage[static_cast<std::size_t>(tile_index(nx, ny))] += coverage[static_cast<std::size_t>(tile_index(nx, ny))] < 255;
            }
        build_out.emit(BuildEvent{.tile_x = x, .tile_y = y, .kind = static_cast<std::uint32_t>(kind)});
        refresh_path();
        return true;
    }

    /// Маршрут: тайлы, по которым идут кратчайшие пути от ворот (по полю потоков).
    void refresh_path() {
        flow.build(map);
        std::ranges::fill(on_path, std::uint8_t{0});
        for (const auto& g : gates) {
            int x = g[0], y = g[1];
            for (int guard = 0; guard < static_cast<int>(tile_count); ++guard) {
                const auto i = static_cast<std::size_t>(tile_index(x, y));
                on_path[i] = 1;
                if (flow.dist[i] == 0 || flow.dist[i] == FlowField::unreachable) break;
                const glm::vec2 d = flow.dir[i];
                x += d.x > 0.3f ? 1 : (d.x < -0.3f ? -1 : 0);
                y += d.y > 0.3f ? 1 : (d.y < -0.3f ? -1 : 0);
            }
        }
    }
};

/// Настройки отображения и режима: T — параллельно / главный поток, F — поле потоков.
struct Settings {
    es::EventReader<Core::KeyEvent> keys;
    bool parallel = true;
    bool show_flow = false;

    void declare(es::EventBus& bus) { keys = bus.reader<Core::KeyEvent>(bus.declare_module("Settings").consumes<Core::KeyEvent>()); }
    void tick() {
        for (const Core::KeyEvent& key : keys.events()) {
            if (key.action != GLFW_PRESS) continue;
            if (key.key == GLFW_KEY_T) parallel = !parallel;
            if (key.key == GLFW_KEY_F) show_flow = !show_flow;
        }
    }
};

// =============================================================================
// Игра
// =============================================================================

class Siege final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {world_w, world_h}; }

    void setup(Core::App& app) override {
        const auto& args = app.config().extra_args;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const bool has_value = i + 1 < args.size();
            if (args[i] == "--no-draw") draw_enemies = false;
            if (args[i] == "--wave" && has_value) waves.first_wave = static_cast<std::uint32_t>(std::strtoul(args[i + 1].c_str(), nullptr, 10));
            if (args[i] == "--cap" && has_value) lifecycle.cap = std::clamp<std::size_t>(std::strtoull(args[i + 1].c_str(), nullptr, 10), 100, 1'000'000);
        }
        es::EventBus& bus = app.bus();
        lifecycle.declare(bus);
        waves.declare(bus);
        navigation.declare(bus);
        targeting.declare(bus);
        ballistics.declare(bus);
        damage.declare(bus);
        economy.declare(bus, app.platform_module());
        settings.declare(bus);
        world.pool<Body>().reserve(lifecycle.cap);
        world.pool<Health>().reserve(lifecycle.cap);
        world.pool<Stats>().reserve(lifecycle.cap);
        if (waves.first_wave > 0) economy.gold += static_cast<std::uint64_t>(Waves::wave_size(waves.first_wave) * 3.0); // догоняем экономику
        std::println("Siege: map {}x{} tiles, start wave {}, enemy cap {}, {} job threads", tiles_x, tiles_y, waves.first_wave,
                     lifecycle.cap, app.jobs().threads());
    }

    void tick(Core::App& app) override {
        const float dt = app.tick_seconds();
        settings.tick();
        js::Scheduler& jobs = settings.parallel ? app.jobs() : serial_jobs;

        profiler.measure(Profiler::Lifecycle, [&] {
            lifecycle.tick(world);
            waves.tick(app.tick());
        });
        profiler.measure(Profiler::Grid, [&] { grid.build(jobs, app.tick_arena(), world, navigation.flow); });
        profiler.measure(Profiler::Navigation, [&] { navigation.tick(jobs, world, grid, dt); });
        profiler.measure(Profiler::Targeting, [&] { targeting.tick(jobs, world, grid); });
        profiler.measure(Profiler::Ballistics, [&] { ballistics.tick(jobs, world, grid, dt); });
        profiler.measure(Profiler::Damage, [&] { damage.tick(world); });
        profiler.measure(Profiler::Economy, [&] { economy.tick(jobs, app.tick()); });
        peak_tick_memory = std::max(peak_tick_memory, app.tick_arena().used());
        ++profiler.ticks;

        if (app.tick() > 0 && app.tick() % 300 == 0) {
            std::println("[tick {:>5}] wave {:>2} | enemies {:>6} | towers {:>4} | projectiles {:>5} | gold {:>6} | castle {:>4} | tick {:6.2f} ms ({})",
                         app.tick(), waves.wave, world.count<Body>(), world.count<Tower>(), ballistics.flying.size(),
                         economy.gold, economy.integrity, profiler.last_tick_ms(), settings.parallel ? "parallel" : "main thread");
        }
    }

    void render(Core::App& /*app*/, Renderer2D& r) override {
        profiler.measure(Profiler::Render, [&] {
            r.fill_rect({{0.0f, 0.0f}, world_size()}, Color::from_rgba(0x1B2416FF), -10);
            const std::vector<Terrain>& map = navigation.map;
            for (int y = 0; y < tiles_y; ++y) {
                for (int x = 0; x < tiles_x; ++x) {
                    const Terrain t = map[static_cast<std::size_t>(tile_index(x, y))];
                    if (t == Terrain::Rock) r.fill_rect({{x * tile, y * tile}, {tile, tile}}, Color::from_rgba(0x55524CFF), -5);
                    if (t == Terrain::Castle) r.fill_rect({{x * tile, y * tile}, {tile, tile}}, Color::from_rgba(0x8D6E63FF), -5);
                }
            }
            for (const auto& g : gates) r.draw_rect({tile_center(g[0], g[1]) - tile, {tile * 2, tile * 2}}, 2.0f, Color::from_rgba(0xEF5350FF), -4);
            if (settings.show_flow) {
                for (int y = 0; y < tiles_y; ++y)
                    for (int x = 0; x < tiles_x; ++x) {
                        const glm::vec2 d = navigation.flow.dir[static_cast<std::size_t>(tile_index(x, y))];
                        if (d.x != 0.0f || d.y != 0.0f)
                            r.draw_line(tile_center(x, y), tile_center(x, y) + d * 6.0f, 1.0f, Color{120, 160, 120, 140}, -4);
                    }
            }
            if (const ECS::ComponentPool<Tower>* towers = world.find_pool<Tower>()) {
                for (const Tower& t : towers->components()) {
                    const bool archer = t.kind == TowerKind::Archer;
                    r.fill_rect({t.position - tile * 0.45f, glm::vec2{tile * 0.9f}}, Color::from_rgba(archer ? 0x42A5F5FF : 0xFFA726FF), 1);
                }
            }
            if (draw_enemies) {
                const ECS::ComponentPool<Body>& bodies = world.pool<Body>();
                const std::span<const Body> b = bodies.components();
                const std::span<const ECS::Entity> e = bodies.entities();
                for (std::size_t i = 0; i < b.size(); ++i) {
                    const Health* h = world.get<Health>(e[i]);
                    const float life = h ? std::clamp(h->hp / h->max_hp, 0.0f, 1.0f) : 0.0f;
                    const auto red = static_cast<std::uint8_t>(120 + 135 * life);
                    r.fill_rect({b[i].position - 1.5f, {3.0f, 3.0f}}, Color{red, static_cast<std::uint8_t>(60 * life), 40, 255}, 0);
                }
            }
            for (const Ballistics::Projectile& p : ballistics.flying) {
                const bool archer = p.kind == TowerKind::Archer;
                r.fill_rect({p.position - (archer ? 1.0f : 2.0f), glm::vec2{archer ? 2.0f : 4.0f}}, archer ? Colors::white : Color::from_rgba(0xFFCC80FF), 2);
            }
        });
        ++profiler.frames;
    }

    void render_overlay(Core::App& app, Renderer2D& r) override {
        profiler.render_overlay(r, app.camera().viewport);
        // Полоска целостности замка.
        const float w = 300.0f * static_cast<float>(std::max<std::int64_t>(economy.integrity, 0)) / 500.0f;
        r.fill_rect({{12.0f, 12.0f}, {300.0f, 10.0f}}, Color{0, 0, 0, 170}, 0);
        r.fill_rect({{12.0f, 12.0f}, {w, 10.0f}}, Color::from_rgba(0x66BB6AFF), 1);
    }

    [[nodiscard]] std::string status() const override {
        return std::format("wave {} | enemies {} | towers {} | gold {} | castle {} | {} | tick {:.2f} ms", waves.wave,
                           world.count<Body>(), world.count<Tower>(), economy.gold, economy.integrity,
                           settings.parallel ? "parallel [T]" : "main thread [T]", profiler.last_tick_ms());
    }

    void shutdown(Core::App& app) override {
        std::println("\n===== Siege : summary, {} ticks, {} frames =====", profiler.ticks, profiler.frames);
        std::println("wave {} | spawned {} (skipped by cap {}) | peak enemies {} | alive {}", waves.wave, lifecycle.spawned,
                     lifecycle.skipped, lifecycle.peak_alive, world.count<Body>());
        std::println("towers {} (archers {}, mortars {}; by player {}) | flow field rebuilds {}", world.count<Tower>(),
                     lifecycle.archers, lifecycle.mortars, economy.player_built, navigation.rebuilds);
        std::println("shots {} | impacts {} | kills {} | wasted hits {} | breaches {} | gold {} (earned {}) | castle {}",
                     targeting.fired, ballistics.impacts_total, damage.kills, damage.wasted, economy.breached, economy.gold,
                     economy.earned, economy.integrity);
        std::println("\n{:<12} {:>10} {:>10}", "system", "ms / tick", "parallel");
        double tick_total = 0.0;
        for (std::size_t s = 0; s < Profiler::Render; ++s) {
            const double ms = Profiler::per(profiler.total_ms[s], profiler.ticks);
            tick_total += ms;
            std::println("{:<12} {:>10.3f} {:>10}", Profiler::names[s], ms, Profiler::parallel[s] ? "yes" : "");
        }
        std::println("{:<12} {:>10.3f}", "TICK TOTAL", tick_total);
        std::println("{:<12} {:>10.3f} ms / frame (CPU submission)", "render", Profiler::per(profiler.total_ms[Profiler::Render], profiler.frames));
        std::println("tick memory peak: {:.2f} MiB (spatial grid, rebuilt every tick, zero malloc)",
                     static_cast<double>(peak_tick_memory) / (1024.0 * 1024.0));

        // Контрольная сумма всего состояния: одинакова при любом --threads.
        std::uint64_t checksum = 1469598103934665603ULL;
        const auto mix = [&](std::uint64_t v) { checksum = (checksum ^ v) * 1099511628211ULL; };
        const auto mixf = [&](float f) { mix(std::bit_cast<std::uint32_t>(f)); };
        for (const Body& b : world.pool<Body>().components()) {
            mixf(b.position.x), mixf(b.position.y), mixf(b.velocity.x), mixf(b.velocity.y);
        }
        for (const Health& h : world.pool<Health>().components()) mixf(h.hp);
        if (const auto* towers = world.find_pool<Tower>())
            for (const Tower& t : towers->components()) mix(t.shots), mix(static_cast<std::uint64_t>(t.cooldown));
        mix(economy.gold), mix(damage.kills), mix(economy.breached), mix(ballistics.flying.size());
        std::println("jobs: {} background threads, {} jobs run | world checksum {:016x}", app.jobs().threads(),
                     app.jobs().stats().jobs_executed, checksum);
    }

private:
    ECS::World world;
    EnemyGrid grid;
    Lifecycle lifecycle;
    Waves waves;
    Navigation navigation;
    Targeting targeting;
    Ballistics ballistics;
    Damage damage;
    Economy economy;
    Settings settings;
    Profiler profiler;
    js::Scheduler serial_jobs{{.threads = 0, .scratch_bytes = ms::KiB(64)}};
    std::size_t peak_tick_memory = 0;
    bool draw_enemies = true;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<Siege>({.title = "Siege", .ticks_per_second = 30.0}, argc, argv);
}
