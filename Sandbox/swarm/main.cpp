/**
 * @file main.cpp
 * @brief Swarm — стенд производительности: десятки и сотни тысяч агентов на всех модулях движка.
 *
 * Каждый тик:
 * 1. Grid      — пространственная сетка (сортировка подсчётом) во временной памяти тика
 *                (MemorySystem: выделение — сдвиг указателя, освобождение — reset после тика);
 * 2. Steering  — стая: разлёт с соседями, выравнивание, притяжение к аттракторам (ЛКМ — к курсору,
 *                ПКМ — от курсора); соседи ищутся только в 3×3 клетках сетки.
 *                **Параллельно** (JobSystem::parallel_for): читает снимок сетки, пишет только свою строку;
 * 3. Movement  — интегрирование по плотному массиву компонентов ECS, тоже параллельно;
 * 4. Hazards   — вращающиеся лезвия сбивают агентов: массовое SoA-событие `swarm.hit`;
 * 0. Population — первым в тике: единственный владелец структуры мира. Уничтожает сбитых
 *                в прошлом тике (устаревшие ссылки отсекает поколение) и рождает новых.
 *                Первым — чтобы сбитые не попадали под лезвия второй раз.
 *
 * Profiler замеряет каждую систему и отрисовку, рисует столбики в оверлее и печатает отчёт
 * каждые 300 тиков и в конце. Детерминирован: `--ticks N` даёт одинаковый результат
 * (и одинаковую контрольную сумму мира) при любом `--threads N`.
 *
 * Управление: [ / ] — вдвое меньше / больше агентов, ЛКМ — притягивать к курсору, ПКМ — отталкивать,
 * T — переключить параллельные системы (app.jobs()) ↔ всё в главном потоке.
 * Аргументы: `--agents N` (по умолчанию 50 000); `--no-draw` — не рисовать агентов (чистые замеры
 * симуляции: без видеокарты программный OpenGL отнимает ядра у тика). Общие клавиши — см. Core::App.
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
#include <format>
#include <numbers>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace es = EventSystem;
namespace ms = MemorySystem;
using namespace RendererSystem;

namespace {

// =============================================================================
// Мир
// =============================================================================

constexpr float world_w = 1920.0f;
constexpr float world_h = 1080.0f;
constexpr float cell = 24.0f;                 ///< Сторона клетки сетки = радиус обзора агента.
constexpr int cells_x = static_cast<int>(world_w / cell);
constexpr int cells_y = static_cast<int>(world_h / cell);
constexpr std::size_t cell_count = static_cast<std::size_t>(cells_x) * cells_y;

constexpr float separation_radius = 7.0f;
constexpr float min_speed = 40.0f;            ///< Единиц мира в секунду.
constexpr float max_speed = 140.0f;
constexpr int max_neighbors = 24;             ///< Предел учтённых соседей.
constexpr int max_candidates = 48;            ///< Предел просмотренных кандидатов: в толпе цена агента не растёт.
constexpr std::size_t min_agents = 1'000;
constexpr std::size_t max_agents = 1'000'000;

int cell_of(glm::vec2 p) {
    const int cx = std::clamp(static_cast<int>(p.x / cell), 0, cells_x - 1);
    const int cy = std::clamp(static_cast<int>(p.y / cell), 0, cells_y - 1);
    return cy * cells_x + cx;
}

// =============================================================================
// Компоненты и события
// =============================================================================

/// Горячие данные агента: 16 байт, лежат плотно в пуле ECS.
struct Body {
    glm::vec2 position{0.0f};
    glm::vec2 velocity{0.0f};
};

/// Лезвие: вращается вокруг центра, сбивает агентов в радиусе.
struct Blade {
    glm::vec2 center{0.0f};
    float orbit = 0.0f;
    float radius = 0.0f;
    float angle = 0.0f;
    float angular_speed = 0.0f;
    [[nodiscard]] glm::vec2 position() const { return center + glm::vec2{std::cos(angle), std::sin(angle)} * orbit; }
};

/// Агента сбило лезвие. Массовое событие — SoA.
struct HitEvent {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    float x = 0.0f;
    float y = 0.0f;

    static constexpr std::string_view event_name = "swarm.hit";
    static constexpr es::Layout layout = es::Layout::SoA;
    using fields = es::Fields<es::Field<"index", &HitEvent::index>, es::Field<"generation", &HitEvent::generation>,
                              es::Field<"x", &HitEvent::x>, es::Field<"y", &HitEvent::y>>;
};

// =============================================================================
// Profiler — замеры по системам
// =============================================================================

struct Profiler {
    enum Section : std::size_t { Grid, Steering, Movement, Hazards, Population, Render, Count };
    static constexpr std::array<std::string_view, Count> names = {"grid", "steering", "movement", "hazards", "population",
                                                                  "render"};
    using Clock = std::chrono::steady_clock;

    std::array<double, Count> window_ms{}; ///< Сумма за текущее окно отчёта.
    std::array<double, Count> total_ms{};  ///< Сумма за всё время.
    std::array<double, Count> last_ms{};   ///< Последний замер (для оверлея).
    std::uint64_t window_ticks = 0;
    std::uint64_t window_frames = 0;
    std::uint64_t total_ticks = 0;
    std::uint64_t total_frames = 0;

    template<typename Fn>
    void measure(Section s, Fn&& fn) {
        const auto start = Clock::now();
        fn();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        window_ms[s] += ms;
        total_ms[s] += ms;
        last_ms[s] = ms;
    }

    [[nodiscard]] static double per(double sum, std::uint64_t n) { return n > 0 ? sum / static_cast<double>(n) : 0.0; }

    [[nodiscard]] double tick_ms(const std::array<double, Count>& sums, std::uint64_t ticks) const {
        double sum = 0.0;
        for (std::size_t s = Grid; s < Render; ++s) sum += sums[s];
        return per(sum, ticks);
    }

    void report(es::Tick now, std::size_t agents, std::uint64_t hits_per_window) {
        std::string line = std::format("[tick {:>5}] {:>7} agents | tick {:6.2f} ms =", now, agents, tick_ms(window_ms, window_ticks));
        for (std::size_t s = Grid; s < Render; ++s) line += std::format(" {} {:.2f}", names[s], per(window_ms[s], window_ticks));
        line += std::format(" | render {:.2f} ms/frame | hits/tick {:.1f}", per(window_ms[Render], window_frames),
                            per(static_cast<double>(hits_per_window), window_ticks));
        std::println("{}", line);
        window_ms = {};
        window_ticks = 0;
        window_frames = 0;
    }

    /// Столбики: высота — миллисекунды последнего замера (1 мс = 20 px), цвет — система.
    void render_overlay(Renderer2D& r, glm::vec2 viewport) const {
        static constexpr std::array<std::uint32_t, Count> colors = {0x4FC3F7FF, 0x81C784FF, 0xFFB74DFF,
                                                                    0xE57373FF, 0xBA68C8FF, 0xFFF176FF};
        const glm::vec2 origin{viewport.x - 12.0f - static_cast<float>(Count) * 18.0f, viewport.y - 12.0f};
        r.fill_rect({{origin.x - 6.0f, origin.y - 206.0f}, {static_cast<float>(Count) * 18.0f + 12.0f, 212.0f}},
                    Color{0, 0, 0, 160}, 0);
        for (int ms = 1; ms <= 10; ++ms) { // шкала: засечка на каждую миллисекунду
            r.fill_rect({{origin.x - 6.0f, origin.y - static_cast<float>(ms) * 20.0f}, {4.0f, 1.0f}}, Colors::white, 1);
        }
        for (std::size_t s = 0; s < Count; ++s) {
            const float h = std::min(static_cast<float>(last_ms[s]) * 20.0f, 200.0f);
            r.fill_rect({{origin.x + static_cast<float>(s) * 18.0f, origin.y - h}, {12.0f, std::max(h, 1.0f)}},
                        Color::from_rgba(colors[s]), 2);
        }
    }
};

// =============================================================================
// Модули
// =============================================================================

/// Сетка, построенная в памяти тика: агенты отсортированы по клеткам, данные скопированы плотно.
struct SpatialGrid {
    std::span<std::uint32_t> cell_start; ///< cell_start[c]..cell_start[c+1] — агенты клетки c в sorted-массивах.
    std::span<glm::vec2> position;       ///< Отсортировано по клеткам.
    std::span<glm::vec2> velocity;
    std::span<std::uint32_t> row;        ///< Строка плотного массива пула Body для каждого отсортированного агента.

    /// Сортировка подсчётом по клеткам. Вся память — из арены тика: ни одного malloc.
    void build(ms::Arena& arena, const ECS::ComponentPool<Body>& bodies) {
        const std::span<const Body> b = bodies.components();
        const std::size_t n = b.size();
        cell_start = arena.push_array<std::uint32_t>(cell_count + 1); // нули: счётчики уже обнулены (ZII)
        position = arena.push_array<glm::vec2>(n);
        velocity = arena.push_array<glm::vec2>(n);
        row = arena.push_array<std::uint32_t>(n);
        std::span<std::uint32_t> cell_of_row = arena.push_array<std::uint32_t>(n);

        for (std::size_t i = 0; i < n; ++i) {
            const auto c = static_cast<std::uint32_t>(cell_of(b[i].position));
            cell_of_row[i] = c;
            ++cell_start[c + 1];
        }
        for (std::size_t c = 0; c < cell_count; ++c) cell_start[c + 1] += cell_start[c];

        // Раскладываем, сдвигая начало каждой клетки; потом восстанавливаем начала.
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t slot = cell_start[cell_of_row[i]]++;
            position[slot] = b[i].position;
            velocity[slot] = b[i].velocity;
            row[slot] = static_cast<std::uint32_t>(i);
        }
        for (std::size_t c = cell_count; c > 0; --c) cell_start[c] = cell_start[c - 1];
        cell_start[0] = 0;
    }
};

/// Steering — стая: читает снимок сетки, пишет скорости. Порядок агентов не влияет на результат.
struct Steering {
    std::array<glm::vec2, 3> attractors{};

    void update_attractors(es::Tick now) {
        const float t = static_cast<float>(now) / 30.0f;
        const glm::vec2 c{world_w * 0.5f, world_h * 0.5f};
        attractors[0] = c + glm::vec2{std::cos(t * 0.31f) * 620.0f, std::sin(t * 0.47f) * 330.0f};
        attractors[1] = c + glm::vec2{std::cos(t * 0.23f + 2.1f) * 520.0f, std::sin(t * 0.37f + 1.3f) * 360.0f};
        attractors[2] = c + glm::vec2{std::sin(t * 0.19f + 4.0f) * 700.0f, std::cos(t * 0.29f + 0.5f) * 280.0f};
    }

    /// Каждый агент читает только снимок сетки и пишет только свою строку пула: куски независимы,
    /// результат не зависит от числа потоков.
    void tick(JobSystem::Scheduler& jobs, ECS::ComponentPool<Body>& bodies, const SpatialGrid& grid,
              const Core::FrameInput& input, float dt) const {
        JobSystem::parallel_for(jobs, grid.position.size(), 1024, [&](std::size_t begin, std::size_t end) {
            steer_range(bodies.components(), grid, input, dt, begin, end);
        });
    }

    void steer_range(std::span<Body> b, const SpatialGrid& grid, const Core::FrameInput& input, float dt,
                     std::size_t begin, std::size_t end) const {
        const bool attract = input.down[0];
        const bool repel = input.down[1];
        const glm::vec2 mouse = input.mouse_world;

        for (std::size_t sorted = begin; sorted < end; ++sorted) {
            const glm::vec2 p = grid.position[sorted];
            const glm::vec2 v = grid.velocity[sorted];
            const int cx = std::clamp(static_cast<int>(p.x / cell), 0, cells_x - 1);
            const int cy = std::clamp(static_cast<int>(p.y / cell), 0, cells_y - 1);

            glm::vec2 separation{0.0f};
            glm::vec2 heading_sum{0.0f};
            glm::vec2 center_sum{0.0f};
            int seen = 0;
            int examined = 0;
            const auto budget_left = [&] { return seen < max_neighbors && examined < max_candidates; };
            for (int y = std::max(cy - 1, 0); y <= std::min(cy + 1, cells_y - 1) && budget_left(); ++y) {
                for (int x = std::max(cx - 1, 0); x <= std::min(cx + 1, cells_x - 1) && budget_left(); ++x) {
                    const std::size_t c = static_cast<std::size_t>(y * cells_x + x);
                    for (std::uint32_t k = grid.cell_start[c]; k < grid.cell_start[c + 1] && budget_left(); ++k) {
                        if (k == sorted) continue;
                        ++examined;
                        const glm::vec2 d = p - grid.position[k];
                        const float dist2 = d.x * d.x + d.y * d.y;
                        if (dist2 > cell * cell) continue;
                        if (dist2 < separation_radius * separation_radius && dist2 > 1e-4f) separation += d / dist2;
                        heading_sum += grid.velocity[k];
                        center_sum += grid.position[k];
                        ++seen;
                    }
                }
            }

            glm::vec2 goal = attractors[0];
            if (attract || repel) {
                goal = mouse;
            } else {
                float best = 1e30f;
                for (const glm::vec2 a : attractors) {
                    const glm::vec2 d = a - p;
                    const float dist2 = d.x * d.x + d.y * d.y;
                    if (dist2 < best) {
                        best = dist2;
                        goal = a;
                    }
                }
            }
            glm::vec2 to_goal = goal - p;
            const float goal_len = std::sqrt(to_goal.x * to_goal.x + to_goal.y * to_goal.y) + 1e-3f;
            to_goal = to_goal / goal_len * (repel ? -1.0f : 1.0f);

            glm::vec2 steer = to_goal * 60.0f + separation * 900.0f;
            if (seen > 0) {
                const float inv = 1.0f / static_cast<float>(seen);
                steer += (heading_sum * inv - v) * 1.5f + (center_sum * inv - p) * 0.8f;
            }
            glm::vec2 next = v + steer * dt;
            const float speed = std::sqrt(next.x * next.x + next.y * next.y) + 1e-4f;
            next *= std::clamp(speed, min_speed, max_speed) / speed;
            b[grid.row[sorted]].velocity = next;
        }
    }
};

/// Movement — интегрирование по плотному массиву пула; мир замкнут (выход справа — вход слева).
struct Movement {
    static void tick(JobSystem::Scheduler& jobs, ECS::ComponentPool<Body>& bodies, float dt) {
        const std::span<Body> all = bodies.components();
        JobSystem::parallel_for(jobs, all.size(), 16384, [&](std::size_t begin, std::size_t end) {
            for (Body& body : all.subspan(begin, end - begin)) {
                body.position += body.velocity * dt;
                if (body.position.x < 0.0f) body.position.x += world_w;
                if (body.position.x >= world_w) body.position.x -= world_w;
                if (body.position.y < 0.0f) body.position.y += world_h;
                if (body.position.y >= world_h) body.position.y -= world_h;
            }
        });
    }
};

/// Hazards — лезвия. Кандидатов берут из сетки: проверяются только клетки под лезвием.
struct Hazards {
    es::EventWriter<HitEvent> hits;

    void declare(es::EventBus& bus) {
        hits = bus.writer<HitEvent>(bus.declare_module("Hazards").produces<HitEvent>(
            es::ChannelConfig{.reserve = 4096, .max_events_per_tick = 65536}));
    }

    void spawn(ECS::World& world) {
        const glm::vec2 c{world_w * 0.5f, world_h * 0.5f};
        for (int i = 0; i < 6; ++i) {
            const ECS::Entity e = world.create();
            world.emplace<Blade>(e, c, 180.0f + 60.0f * static_cast<float>(i), 26.0f + 4.0f * static_cast<float>(i % 3),
                                 static_cast<float>(i) * 1.1f, (i % 2 == 0 ? 0.9f : -0.7f) / (1.0f + static_cast<float>(i) * 0.15f));
        }
    }

    void tick(ECS::World& world, const ECS::ComponentPool<Body>& bodies, const SpatialGrid& grid, float dt) {
        const std::span<const ECS::Entity> entities = bodies.entities();
        world.view<Blade>().each([&](Blade& blade) {
            blade.angle += blade.angular_speed * dt;
            const glm::vec2 at = blade.position();
            const int x0 = std::max(static_cast<int>((at.x - blade.radius) / cell), 0);
            const int x1 = std::min(static_cast<int>((at.x + blade.radius) / cell), cells_x - 1);
            const int y0 = std::max(static_cast<int>((at.y - blade.radius) / cell), 0);
            const int y1 = std::min(static_cast<int>((at.y + blade.radius) / cell), cells_y - 1);
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const std::size_t c = static_cast<std::size_t>(y * cells_x + x);
                    for (std::uint32_t k = grid.cell_start[c]; k < grid.cell_start[c + 1]; ++k) {
                        const glm::vec2 d = grid.position[k] - at;
                        if (d.x * d.x + d.y * d.y > blade.radius * blade.radius) continue;
                        const ECS::Entity victim = entities[grid.row[k]];
                        hits.emit(HitEvent{.index = victim.index, .generation = victim.generation,
                                           .x = grid.position[k].x, .y = grid.position[k].y});
                    }
                }
            }
        });
    }
};

/// Population — единственный, кто создаёт и уничтожает агентов. Держит заданную численность.
struct Population {
    es::EventReader<HitEvent> hits;
    es::EventReader<Core::KeyEvent> keys;
    std::size_t target = 50'000;
    std::uint64_t destroyed = 0;
    std::uint64_t stale = 0;          ///< Попадания по уже уничтоженному агенту (задело два лезвия сразу).
    std::uint64_t window_hits = 0;
    bool parallel = true;             ///< T: системы на app.jobs() или в главном потоке.
    std::mt19937 rng{2718};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Population").consumes<HitEvent>().consumes<Core::KeyEvent>();
        hits = bus.reader<HitEvent>(id);
        keys = bus.reader<Core::KeyEvent>(id);
    }

    void tick(ECS::World& world) {
        for (const Core::KeyEvent& key : keys.events()) {
            if (key.action != GLFW_PRESS) continue;
            if (key.key == GLFW_KEY_T) parallel = !parallel;
            if (key.key == GLFW_KEY_LEFT_BRACKET) target = std::max(target / 2, min_agents);
            if (key.key == GLFW_KEY_RIGHT_BRACKET) target = std::min(target * 2, max_agents);
        }

        // Сбитые: SoA-колонки события, сущность восстанавливается из двух полей.
        const auto index = hits.column<&HitEvent::index>();
        const auto generation = hits.column<&HitEvent::generation>();
        window_hits += index.size();
        for (std::size_t k = 0; k < index.size(); ++k) {
            if (world.destroy(ECS::Entity{index[k], generation[k]})) {
                ++destroyed;
            } else {
                ++stale;
            }
        }

        auto& bodies = world.pool<Body>();
        while (bodies.size() > target) world.destroy(bodies.entities().back());
        if (bodies.size() < target) spawn(world, target - bodies.size());
    }

    void spawn(ECS::World& world, std::size_t count) {
        world.pool<Body>().reserve(world.count<Body>() + count);
        std::uniform_real_distribution<float> x(0.0f, world_w - 0.01f);
        std::uniform_real_distribution<float> y(0.0f, world_h - 0.01f);
        std::uniform_real_distribution<float> angle(0.0f, 2.0f * std::numbers::pi_v<float>);
        for (std::size_t i = 0; i < count; ++i) {
            const float a = angle(rng);
            const ECS::Entity e = world.create();
            world.emplace<Body>(e, glm::vec2{x(rng), y(rng)}, glm::vec2{std::cos(a), std::sin(a)} * min_speed);
        }
    }
};

// =============================================================================
// Игра
// =============================================================================

class Swarm final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {world_w, world_h}; }

    void setup(Core::App& app) override {
        const auto& args = app.config().extra_args;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--no-draw") draw_agents = false;
            if (args[i] == "--agents" && i + 1 < args.size()) {
                population.target = std::clamp<std::size_t>(std::strtoull(args[i + 1].c_str(), nullptr, 10), min_agents, max_agents);
            }
        }
        hazards.declare(app.bus());
        population.declare(app.bus());
        hazards.spawn(world);
        population.spawn(world, population.target);
        std::println("Swarm: {} agents, grid {}x{} cells of {} units, {} job threads", population.target, cells_x,
                     cells_y, cell, app.jobs().threads());
    }

    void tick(Core::App& app) override {
        const float dt = app.tick_seconds();
        auto& bodies = world.pool<Body>();
        ms::Arena& scratch = app.tick_arena();
        // Тот же код систем, другой планировщик: без фоновых потоков parallel_for идёт в главном потоке.
        JobSystem::Scheduler& jobs = population.parallel ? app.jobs() : serial_jobs;

        // Сначала структура мира: сбитые в прошлом тике исчезают до того, как лезвия посмотрят снова.
        profiler.measure(Profiler::Population, [&] { population.tick(world); });
        profiler.measure(Profiler::Grid, [&] { grid.build(scratch, bodies); });
        profiler.measure(Profiler::Steering, [&] {
            steering.update_attractors(app.tick());
            steering.tick(jobs, bodies, grid, app.input(), dt);
        });
        profiler.measure(Profiler::Movement, [&] { Movement::tick(jobs, bodies, dt); });
        profiler.measure(Profiler::Hazards, [&] { hazards.tick(world, bodies, grid, dt); });
        peak_tick_memory = std::max(peak_tick_memory, scratch.used());

        ++profiler.window_ticks;
        ++profiler.total_ticks;
        if (app.tick() > 0 && app.tick() % 300 == 0) {
            profiler.report(app.tick(), world.count<Body>(), population.window_hits);
            population.window_hits = 0;
        }
    }

    void render(Core::App& /*app*/, Renderer2D& r) override {
        profiler.measure(Profiler::Render, [&] {
            r.fill_rect({{0.0f, 0.0f}, world_size()}, Color::from_rgba(0x0B0E14FF), -10);
            // Цвет — от скорости: медленные синие, быстрые светлые. Обход плотного массива пула.
            for (const Body& body : draw_agents ? world.pool<Body>().components() : std::span<Body>()) {
                const float speed = std::sqrt(body.velocity.x * body.velocity.x + body.velocity.y * body.velocity.y);
                const auto t = static_cast<std::uint8_t>(std::clamp((speed - min_speed) / (max_speed - min_speed), 0.0f, 1.0f) * 255.0f);
                r.fill_rect({body.position - 1.5f, {3.0f, 3.0f}}, Color{static_cast<std::uint8_t>(60 + t * 3 / 4), static_cast<std::uint8_t>(110 + t / 2), 255, 255}, 0);
            }
            world.view<const Blade>().each([&](const Blade& blade) {
                r.draw(SpriteInstance{.position = blade.position(), .size = glm::vec2(blade.radius * 1.6f),
                                      .rotation = blade.angle * 4.0f, .color = Color::from_rgba(0xFF5A4AE0), .layer = 2});
            });
            for (const glm::vec2 a : steering.attractors) {
                r.draw_rect({a - 6.0f, {12.0f, 12.0f}}, 2.0f, Color::from_rgba(0xFFD54FFF), 3);
            }
        });
        ++profiler.window_frames;
        ++profiler.total_frames;
    }

    void render_overlay(Core::App& app, Renderer2D& r) override { profiler.render_overlay(r, app.camera().viewport); }

    [[nodiscard]] std::string status() const override {
        return std::format("{} agents | {} | tick {:.2f} ms (steering {:.2f}) | render {:.2f} ms", world.count<Body>(),
                           population.parallel ? "parallel [T]" : "main thread [T]",
                           profiler.last_ms[0] + profiler.last_ms[1] + profiler.last_ms[2] + profiler.last_ms[3] +
                               profiler.last_ms[4],
                           profiler.last_ms[Profiler::Steering], profiler.last_ms[Profiler::Render]);
    }

    void shutdown(Core::App& app) override {
        const auto agents = static_cast<double>(world.count<Body>());
        std::println("\n===== Swarm : performance summary, {} ticks, {} frames, {} agents =====", profiler.total_ticks,
                     profiler.total_frames, world.count<Body>());
        std::println("{:<12} {:>10} {:>14}", "system", "ms / tick", "ns / agent");
        for (std::size_t s = Profiler::Grid; s < Profiler::Render; ++s) {
            const double ms = Profiler::per(profiler.total_ms[s], profiler.total_ticks);
            std::println("{:<12} {:>10.3f} {:>14.1f}", Profiler::names[s], ms, ms * 1e6 / std::max(agents, 1.0));
        }
        const double tick_ms = profiler.tick_ms(profiler.total_ms, profiler.total_ticks);
        std::println("{:<12} {:>10.3f} {:>14.1f}", "TICK TOTAL", tick_ms, tick_ms * 1e6 / std::max(agents, 1.0));
        std::println("{:<12} {:>10.3f} ms / frame (CPU submission; GPU time not included)", "render",
                     Profiler::per(profiler.total_ms[Profiler::Render], profiler.total_frames));
        std::println("budget: a 30 Hz tick has {:.1f} ms -> simulation uses {:.1f}%", 1000.0 / 30.0, tick_ms / (1000.0 / 30.0) * 100.0);
        std::println("ECS: {} alive, {} slots ever used; destroyed {}, stale hits {}", world.alive(),
                     world.registry().slots(), population.destroyed, population.stale);
        std::println("tick memory peak: {:.2f} MiB (allocated and freed every tick, zero malloc)",
                     static_cast<double>(peak_tick_memory) / (1024.0 * 1024.0));
        // Контрольная сумма мира: одинакова при любом --threads (см. Sandbox/README.md).
        std::uint64_t checksum = 1469598103934665603ULL;
        for (const Body& body : world.pool<Body>().components()) {
            for (const float f : {body.position.x, body.position.y, body.velocity.x, body.velocity.y}) {
                checksum = (checksum ^ std::bit_cast<std::uint32_t>(f)) * 1099511628211ULL;
            }
        }
        std::println("jobs: {} background threads, {} jobs run; world checksum {:016x}", app.jobs().threads(),
                     app.jobs().stats().jobs_executed, checksum);
    }

private:
    ECS::World world;
    SpatialGrid grid;
    Steering steering;
    Hazards hazards;
    Population population;
    Profiler profiler;
    JobSystem::Scheduler serial_jobs{{.threads = 0, .scratch_bytes = ms::KiB(64)}};
    std::size_t peak_tick_memory = 0;
    bool draw_agents = true;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<Swarm>({.title = "Swarm", .ticks_per_second = 30.0}, argc, argv);
}
