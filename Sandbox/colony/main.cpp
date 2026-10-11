/**
 * @file main.cpp
 * @brief Colony — упрощённый RimWorld на ECS: колонисты рубят деревья, добывают камень и носят всё на склад.
 *
 * Что показывает:
 * - колонисты и ресурсы — сущности ECS; ссылка «колонист → ресурс» — ECS::Entity с поколением.
 *   Исчерпанный ресурс уничтожается, и все, кто на него ссылался, узнают это через `world.valid()`:
 *   не нужно ни «никогда не переиспользовать индексы», ни рассылать отмену брони;
 * - владельцы данных: World создаёт и уничтожает ресурсы, Colonists — колонистов, Jobs владеет
 *   компонентами брони (Reserved, AwaitingJob). Чужое читается через выборки `const`;
 * - карта уровня в арене MemorySystem: тайлы и «кто стоит на тайле» (ECS::Entity{} = свободно, ZII);
 * - порядок модулей в тике не важен для корректности: всё, что отправлено в тике N, видно в N+1.
 *
 *   Orders ──order_place──▶ World ──resource_spawned / resource_depleted──▶ Chronicle
 *                            ▲
 *                 resource_harvested
 *                            │
 *   Jobs ──job_assigned──▶ Colonists ──item_delivered──▶ Economy ──milestone──▶ Chronicle
 *     ▲                        │
 *     └──resource_harvested────┘  (снять бронь, чтобы ресурс мог взять другой)
 *
 * Управление: ЛКМ — посадить дерево, ПКМ — положить камень, J — линии заданий.
 * Общие клавиши — см. Core::App.
 */

#include <Core/Core.hpp>
#include <ECSSystem/ECSSystem.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <print>
#include <random>
#include <span>
#include <string_view>
#include <vector>

using InputSystem::Key;
using InputSystem::MouseButton;

namespace es = EventSystem;
using namespace RendererSystem;

namespace {

// =============================================================================
// Мир
// =============================================================================

constexpr int map_w = 48;
constexpr int map_h = 30;
constexpr float tile = 16.0f;
constexpr Rect stockpile_tiles{{3.0f, 3.0f}, {6.0f, 4.0f}}; // в тайлах
constexpr int colonist_count = 10;
constexpr float colonist_speed = 48.0f; // пикселей в игровую секунду

enum class ResourceKind : std::uint32_t { Tree = 0, Rock = 1 };

std::string_view kind_name(ResourceKind kind) { return kind == ResourceKind::Tree ? "logs" : "stones"; }

/// Атлас 8×2 клеток по 16 px: всё рисуется одной текстурой.
enum AtlasCell : int {
    Grass = 0, Dirt = 1, StockpileFloor = 2, TreeSprite = 3, RockSprite = 4, LogItem = 5, StoneItem = 6,
    WalkFirst = 8, WorkFirst = 12, Idle = 14,
};

Image make_atlas() {
    Image atlas(128, 32, Colors::transparent);
    auto cell = [&](int index, int x, int y, int w, int h, std::uint32_t rgba) {
        atlas.fill_rect((index % 8) * 16 + x, (index / 8) * 16 + y, w, h, Color::from_rgba(rgba));
    };
    cell(Grass, 0, 0, 16, 16, 0x4E7A3AFF);
    cell(Grass, 3, 4, 1, 2, 0x5E8C46FF);
    cell(Grass, 11, 9, 1, 2, 0x5E8C46FF);
    cell(Dirt, 0, 0, 16, 16, 0x6E5436FF);
    cell(Dirt, 5, 7, 2, 1, 0x5C452CFF);
    cell(StockpileFloor, 0, 0, 16, 16, 0x8C7B5AFF);
    cell(StockpileFloor, 1, 1, 14, 14, 0x9C8B68FF);
    cell(TreeSprite, 7, 9, 2, 6, 0x6B4423FF);
    cell(TreeSprite, 3, 1, 10, 9, 0x2E6B2EFF);
    cell(TreeSprite, 5, 2, 4, 3, 0x3F8A3FFF);
    cell(RockSprite, 2, 5, 12, 9, 0x6D6D75FF);
    cell(RockSprite, 4, 6, 4, 3, 0x8E8E98FF);
    cell(LogItem, 2, 7, 12, 4, 0x8B5A2BFF);
    cell(LogItem, 12, 7, 2, 4, 0xC49A6CFF);
    cell(StoneItem, 4, 6, 8, 6, 0x9A9AA3FF);
    // Колонист: 4 кадра ходьбы, 2 кадра работы, 1 кадр покоя.
    for (int f = 0; f < 7; ++f) {
        const int index = WalkFirst + f;
        cell(index, 6, 1, 4, 4, 0xF1C27DFF);  // голова
        cell(index, 5, 5, 6, 6, 0x3F6FD8FF);  // рубаха
        const int stride = f < 4 ? (f % 2 == 0 ? 1 : -1) : 0;
        cell(index, 5 + stride, 11, 2, 4, 0x2F2F3AFF); // ноги
        cell(index, 9 - stride, 11, 2, 4, 0x2F2F3AFF);
        if (f == 4) cell(index, 11, 2, 2, 7, 0xB0B0B8FF); // инструмент поднят
        if (f == 5) cell(index, 11, 7, 5, 2, 0xB0B0B8FF); // инструмент опущен
    }
    return atlas;
}

UvRect atlas_uv(int index) {
    // make_grid_frames возвращает vector, поэтому UV клеток считаются один раз.
    static const std::vector<AnimationFrame> cells = make_grid_frames({.columns = 8, .rows = 2}, 0, 16, 1.0f);
    return cells[static_cast<std::size_t>(index)].uv;
}

glm::vec2 tile_center(glm::ivec2 t) {
    return {(static_cast<float>(t.x) + 0.5f) * tile, (static_cast<float>(t.y) + 0.5f) * tile};
}

bool in_stockpile(glm::ivec2 t) {
    return stockpile_tiles.contains({static_cast<float>(t.x) + 0.5f, static_cast<float>(t.y) + 0.5f});
}

bool on_map(glm::ivec2 t) { return t.x >= 0 && t.y >= 0 && t.x < map_w && t.y < map_h; }

std::string game_time(es::Tick tick) {
    // Один тик — одна игровая минута.
    return std::format("day {}, {:02}:{:02}", tick / 1440 + 1, (tick / 60) % 24, tick % 60);
}

ECS::Entity entity(std::uint32_t index, std::uint32_t generation) { return {index, generation}; }

// =============================================================================
// Компоненты. Нулевое значение каждого — осмысленное (ZII).
// =============================================================================

/// Ресурс на карте (дерево или камень).
struct Resource {
    ResourceKind kind = ResourceKind::Tree;
    int amount = 0;     ///< Сколько раз ещё можно добыть.
    int max_amount = 0;
};
struct Tile {
    glm::ivec2 at{0};
};

/// Колонист: конечный автомат.
enum class State : std::uint8_t { Idle, ToResource, Working, ToStockpile };
struct Worker {
    State state = State::Idle;
    ECS::Entity target;     ///< Ресурс, над которым работает; Entity{} — нет цели.
    int work_timer = 0;
    bool facing_left = false;
};
struct Position {
    glm::vec2 now{0.0f};
    glm::vec2 goal{0.0f};
};
struct Carrying {             ///< Есть только у тех, кто несёт груз на склад.
    ResourceKind kind = ResourceKind::Tree;
};
struct Anim {
    AnimationState state{};
};

/// Компоненты модуля Jobs: бронь ресурса и «задание отправлено, ждём, пока Colonists его примет».
struct Reserved {
    ECS::Entity by;
};
struct AwaitingJob {
    es::Tick since = 0;
};

// =============================================================================
// События. Сущности передаются двумя полями: события — плоские структуры.
// =============================================================================

struct PlaceOrderEvent {
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    std::uint32_t kind = 0;

    static constexpr std::string_view event_name = "colony.order_place";
    using fields = es::Fields<es::Field<"tile_x", &PlaceOrderEvent::tile_x>, es::Field<"tile_y", &PlaceOrderEvent::tile_y>,
                              es::Field<"kind", &PlaceOrderEvent::kind>>;
};

struct ResourceSpawnedEvent {
    std::uint32_t resource_index = 0;
    std::uint32_t resource_generation = 0;
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    std::uint32_t kind = 0;
    std::uint32_t regrown = 0; ///< 1 — выросло само, 0 — по приказу игрока.

    static constexpr std::string_view event_name = "colony.resource_spawned";
    using fields = es::Fields<es::Field<"resource_index", &ResourceSpawnedEvent::resource_index>,
                              es::Field<"resource_generation", &ResourceSpawnedEvent::resource_generation>,
                              es::Field<"tile_x", &ResourceSpawnedEvent::tile_x>,
                              es::Field<"tile_y", &ResourceSpawnedEvent::tile_y>,
                              es::Field<"kind", &ResourceSpawnedEvent::kind>,
                              es::Field<"regrown", &ResourceSpawnedEvent::regrown>>;
};

struct JobAssignedEvent {
    std::uint32_t colonist_index = 0;
    std::uint32_t colonist_generation = 0;
    std::uint32_t resource_index = 0;
    std::uint32_t resource_generation = 0;

    [[nodiscard]] ECS::Entity colonist() const { return entity(colonist_index, colonist_generation); }
    [[nodiscard]] ECS::Entity resource() const { return entity(resource_index, resource_generation); }

    static constexpr std::string_view event_name = "colony.job_assigned";
    using fields = es::Fields<es::Field<"colonist_index", &JobAssignedEvent::colonist_index>,
                              es::Field<"colonist_generation", &JobAssignedEvent::colonist_generation>,
                              es::Field<"resource_index", &JobAssignedEvent::resource_index>,
                              es::Field<"resource_generation", &JobAssignedEvent::resource_generation>>;
};

struct ResourceHarvestedEvent {
    std::uint32_t resource_index = 0;
    std::uint32_t resource_generation = 0;
    std::uint32_t kind = 0;
    std::uint32_t amount = 0;

    [[nodiscard]] ECS::Entity resource() const { return entity(resource_index, resource_generation); }

    static constexpr std::string_view event_name = "colony.resource_harvested";
    using fields = es::Fields<es::Field<"resource_index", &ResourceHarvestedEvent::resource_index>,
                              es::Field<"resource_generation", &ResourceHarvestedEvent::resource_generation>,
                              es::Field<"kind", &ResourceHarvestedEvent::kind>,
                              es::Field<"amount", &ResourceHarvestedEvent::amount>>;
};

struct ResourceDepletedEvent {
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    std::uint32_t kind = 0;

    static constexpr std::string_view event_name = "colony.resource_depleted";
    using fields = es::Fields<es::Field<"tile_x", &ResourceDepletedEvent::tile_x>,
                              es::Field<"tile_y", &ResourceDepletedEvent::tile_y>,
                              es::Field<"kind", &ResourceDepletedEvent::kind>>;
};

struct ItemDeliveredEvent {
    std::uint32_t kind = 0;
    std::uint32_t amount = 0;

    static constexpr std::string_view event_name = "colony.item_delivered";
    using fields = es::Fields<es::Field<"kind", &ItemDeliveredEvent::kind>, es::Field<"amount", &ItemDeliveredEvent::amount>>;
};

struct MilestoneEvent {
    std::uint32_t kind = 0;
    std::uint32_t total = 0;

    static constexpr std::string_view event_name = "colony.milestone";
    using fields = es::Fields<es::Field<"kind", &MilestoneEvent::kind>, es::Field<"total", &MilestoneEvent::total>>;
};

// =============================================================================
// Модули
// =============================================================================

/// Orders — ввод игрока превращается в приказы.
struct Orders {
    es::EventReader<Core::KeyEvent> keys;
    es::EventReader<Core::MouseButtonEvent> mouse;
    es::EventWriter<PlaceOrderEvent> out;
    bool show_jobs = true;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Orders")
                                    .consumes<Core::KeyEvent>()
                                    .consumes<Core::MouseButtonEvent>()
                                    .produces<PlaceOrderEvent>();
        keys = bus.reader<Core::KeyEvent>(id);
        mouse = bus.reader<Core::MouseButtonEvent>(id);
        out = bus.writer<PlaceOrderEvent>(id);
    }

    void tick() {
        for (const Core::KeyEvent& key : keys.events()) {
            if (key.pressed() && key.code() == Key::J) show_jobs = !show_jobs;
        }
        for (const Core::MouseButtonEvent& click : mouse.events()) {
            if (!click.pressed() || click.which() == MouseButton::Middle) continue;
            out.emit(PlaceOrderEvent{
                .tile_x = static_cast<std::int32_t>(std::floor(click.world_x / tile)),
                .tile_y = static_cast<std::int32_t>(std::floor(click.world_y / tile)),
                .kind = static_cast<std::uint32_t>(click.which() == MouseButton::Left ? ResourceKind::Tree : ResourceKind::Rock),
            });
        }
    }
};

/// World — карта и ресурсы. Единственный, кто создаёт и уничтожает сущности ресурсов.
struct World {
    es::EventReader<PlaceOrderEvent> orders;
    es::EventReader<ResourceHarvestedEvent> harvested;
    es::EventWriter<ResourceSpawnedEvent> spawned;
    es::EventWriter<ResourceDepletedEvent> depleted;

    // Карта уровня живёт в арене: обе таблицы нулевые с рождения (ZII).
    MemorySystem::Arena level = MemorySystem::Arena::reserve(MemorySystem::MiB(1));
    std::span<std::uint8_t> terrain;       ///< AtlasCell тайла.
    std::span<ECS::Entity> tile_owner;     ///< Ресурс на тайле; Entity{} — свободно. Проверка занятости O(1).

    struct Regrowth {
        es::Tick due;
        glm::ivec2 near;
    };
    std::vector<Regrowth> regrowth; // отложенных событий в шине нет — очередь ведётся вручную
    std::mt19937 rng{2024};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("World")
                                    .consumes<PlaceOrderEvent>()
                                    .consumes<ResourceHarvestedEvent>()
                                    .produces<ResourceSpawnedEvent>()
                                    .produces<ResourceDepletedEvent>();
        orders = bus.reader<PlaceOrderEvent>(id);
        harvested = bus.reader<ResourceHarvestedEvent>(id);
        spawned = bus.writer<ResourceSpawnedEvent>(id);
        depleted = bus.writer<ResourceDepletedEvent>(id);
    }

    void generate(ECS::World& world) {
        terrain = level.push_array<std::uint8_t>(map_w * map_h);
        tile_owner = level.push_array<ECS::Entity>(map_w * map_h);
        for (std::uint8_t& cell : terrain) {
            cell = static_cast<std::uint8_t>(std::uniform_int_distribution<int>(0, 9)(rng) == 0 ? Dirt : Grass);
        }
        std::uniform_int_distribution<int> x(0, map_w - 1);
        std::uniform_int_distribution<int> y(0, map_h - 1);
        for (int i = 0; i < 40; ++i) {
            spawn(world, {x(rng), y(rng)}, i < 28 ? ResourceKind::Tree : ResourceKind::Rock, false);
        }
    }

    [[nodiscard]] ECS::Entity& owner(glm::ivec2 t) { return tile_owner[static_cast<std::size_t>(t.y * map_w + t.x)]; }

    /// Создаёт ресурс; Entity{}, если тайл занят или вне карты.
    ECS::Entity spawn(ECS::World& world, glm::ivec2 t, ResourceKind kind, bool announce, bool regrown = false) {
        if (!on_map(t) || in_stockpile(t) || owner(t)) return {};
        const int amount = kind == ResourceKind::Tree ? 3 : 5;
        const ECS::Entity e = world.create();
        world.emplace<Resource>(e, kind, amount, amount);
        world.emplace<Tile>(e, t);
        owner(t) = e;
        if (announce) {
            spawned.emit(ResourceSpawnedEvent{.resource_index = e.index, .resource_generation = e.generation,
                                              .tile_x = t.x, .tile_y = t.y, .kind = static_cast<std::uint32_t>(kind),
                                              .regrown = regrown ? 1u : 0u});
        }
        return e;
    }

    void tick(ECS::World& world, es::Tick now) {
        for (const PlaceOrderEvent& order : orders.events()) {
            spawn(world, {order.tile_x, order.tile_y}, static_cast<ResourceKind>(order.kind), true);
        }
        for (const ResourceHarvestedEvent& h : harvested.events()) {
            Resource* res = world.get<Resource>(h.resource());
            if (res == nullptr) continue; // ресурс уже исчерпан другим колонистом
            res->amount -= static_cast<int>(h.amount);
            if (res->amount > 0) continue;

            const glm::ivec2 at = world.get<Tile>(h.resource())->at;
            const ResourceKind kind = res->kind;
            depleted.emit(ResourceDepletedEvent{.tile_x = at.x, .tile_y = at.y, .kind = static_cast<std::uint32_t>(kind)});
            owner(at) = ECS::Entity{};
            world.destroy(h.resource()); // бронь (Reserved) исчезает вместе с сущностью; ссылки колонистов протухают
            if (kind == ResourceKind::Tree) regrowth.push_back(Regrowth{.due = now + 900, .near = at});
        }
        std::uniform_int_distribution<int> jitter(-3, 3);
        std::erase_if(regrowth, [&](const Regrowth& g) {
            if (g.due > now) return false;
            spawn(world, g.near + glm::ivec2{jitter(rng), jitter(rng)}, ResourceKind::Tree, true, true);
            return true; // занятый тайл — саженец просто не вырос
        });
    }
};

/// Economy — склад и пороги.
struct Economy {
    es::EventReader<ItemDeliveredEvent> delivered;
    es::EventWriter<MilestoneEvent> milestones;
    std::array<std::uint32_t, 2> stock{};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Economy").consumes<ItemDeliveredEvent>().produces<MilestoneEvent>();
        delivered = bus.reader<ItemDeliveredEvent>(id);
        milestones = bus.writer<MilestoneEvent>(id);
    }

    [[nodiscard]] std::uint32_t amount(ResourceKind kind) const { return stock[static_cast<std::size_t>(kind)]; }

    void tick() {
        for (const ItemDeliveredEvent& item : delivered.events()) {
            std::uint32_t& value = stock[item.kind];
            const std::uint32_t before = value;
            value += item.amount;
            if (value / 10 != before / 10) milestones.emit(MilestoneEvent{.kind = item.kind, .total = value});
        }
    }
};

/// Jobs — кому что рубить. Владеет компонентами Reserved (на ресурсах) и AwaitingJob (на колонистах).
struct Jobs {
    es::EventReader<ResourceHarvestedEvent> harvested;
    es::EventWriter<JobAssignedEvent> out;
    std::uint64_t assigned = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Jobs").consumes<ResourceHarvestedEvent>().produces<JobAssignedEvent>();
        harvested = bus.reader<ResourceHarvestedEvent>(id);
        out = bus.writer<JobAssignedEvent>(id);
    }

    void tick(ECS::World& world, const Economy& economy, es::Tick now) {
        // Добыча завершена — ресурс свободен для следующего колониста.
        for (const ResourceHarvestedEvent& h : harvested.events()) world.remove<Reserved>(h.resource());

        // Нужнее тот ресурс, которого на складе меньше.
        const ResourceKind wanted =
            economy.amount(ResourceKind::Tree) <= economy.amount(ResourceKind::Rock) ? ResourceKind::Tree : ResourceKind::Rock;
        auto resources = world.view<const Resource, const Tile>();

        world.view<const Worker, const Position>().each([&](ECS::Entity colonist, const Worker& w, const Position& p) {
            if (w.state != State::Idle) {
                world.remove<AwaitingJob>(colonist);
                return;
            }
            // Задание уже отправлено: ждём, пока Colonists его примет. Если за 2 тика не принял
            // (ресурс исчез раньше), отправляем новое — иначе колонист застрял бы навсегда.
            if (const AwaitingJob* waiting = world.get<AwaitingJob>(colonist); waiting != nullptr && now - waiting->since <= 2) {
                return;
            }

            ECS::Entity best{};
            float best_score = std::numeric_limits<float>::max();
            resources.each([&](ECS::Entity r, const Resource& res, const Tile& t) {
                if (world.has<Reserved>(r)) return;
                const glm::vec2 d = tile_center(t.at) - p.now;
                const float score = std::sqrt(d.x * d.x + d.y * d.y) * (res.kind == wanted ? 1.0f : 2.5f);
                if (score < best_score) {
                    best_score = score;
                    best = r;
                }
            });
            if (!best) return;
            world.emplace<Reserved>(best, colonist);
            world.emplace<AwaitingJob>(colonist, now);
            out.emit(JobAssignedEvent{.colonist_index = colonist.index, .colonist_generation = colonist.generation,
                                      .resource_index = best.index, .resource_generation = best.generation});
            ++assigned;
        });
    }
};

/// Colonists — конечный автомат колонистов. Создаёт колонистов и меняет только их компоненты.
struct Colonists {
    es::EventReader<JobAssignedEvent> jobs;
    es::EventWriter<ResourceHarvestedEvent> harvested;
    es::EventWriter<ItemDeliveredEvent> delivered;
    AnimationLibrary anims;
    ClipId walk;
    ClipId work;
    ClipId idle;
    std::mt19937 rng{7};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Colonists")
                                    .consumes<JobAssignedEvent>()
                                    .produces<ResourceHarvestedEvent>()
                                    .produces<ItemDeliveredEvent>();
        jobs = bus.reader<JobAssignedEvent>(id);
        harvested = bus.writer<ResourceHarvestedEvent>(id);
        delivered = bus.writer<ItemDeliveredEvent>(id);

        const SpriteSheetGrid grid{.columns = 8, .rows = 2};
        walk = anims.add({.name = "colonist.walk", .frames = make_grid_frames(grid, WalkFirst, 4, 0.12f)});
        work = anims.add({.name = "colonist.work", .frames = make_grid_frames(grid, WorkFirst, 2, 0.2f)});
        idle = anims.add({.name = "colonist.idle", .frames = make_grid_frames(grid, Idle, 1, 1.0f)});
    }

    void spawn(ECS::World& world) {
        std::uniform_real_distribution<float> offset(-20.0f, 20.0f);
        const glm::vec2 home = stockpile_tiles.center() * tile;
        for (int i = 0; i < colonist_count; ++i) {
            const ECS::Entity e = world.create();
            const glm::vec2 at = home + glm::vec2{offset(rng), offset(rng)};
            world.emplace<Worker>(e);
            world.emplace<Position>(e, at, at);
            world.emplace<Anim>(e, AnimationState::start(idle));
        }
    }

    void tick(ECS::World& world, float dt) {
        for (const JobAssignedEvent& job : jobs.events()) {
            Worker* w = world.get<Worker>(job.colonist());
            const Tile* target = world.get<Tile>(job.resource());
            if (w == nullptr || w->state != State::Idle || target == nullptr) continue; // ресурс мог исчезнуть
            w->state = State::ToResource;
            w->target = job.resource();
            world.get<Position>(job.colonist())->goal = tile_center(target->at) + glm::vec2{-10.0f, 2.0f};
        }

        std::uniform_int_distribution<int> slot_x(0, static_cast<int>(stockpile_tiles.size.x) - 1);
        std::uniform_int_distribution<int> slot_y(0, static_cast<int>(stockpile_tiles.size.y) - 1);
        world.view<Worker, Position, Anim>().each([&](ECS::Entity e, Worker& w, Position& p, Anim& anim) {
            // Цель исчезла (её исчерпал другой колонист)? Поколение скажет об этом без всяких событий.
            if ((w.state == State::ToResource || w.state == State::Working) && !world.valid(w.target)) {
                w.state = State::Idle;
                w.target = ECS::Entity{};
            }

            const bool moving = w.state == State::ToResource || w.state == State::ToStockpile;
            bool arrived = false;
            if (moving) {
                const glm::vec2 d = p.goal - p.now;
                const float distance = std::sqrt(d.x * d.x + d.y * d.y);
                const float step = colonist_speed * dt;
                if (distance <= step) {
                    p.now = p.goal;
                    arrived = true;
                } else {
                    p.now += d / distance * step;
                    w.facing_left = d.x < 0.0f;
                }
            }

            switch (w.state) {
                case State::ToResource:
                    if (arrived) {
                        w.state = State::Working;
                        w.facing_left = false;
                        w.work_timer = world.get<Resource>(w.target)->kind == ResourceKind::Tree ? 45 : 70;
                    }
                    break;
                case State::Working:
                    if (--w.work_timer <= 0) {
                        const ResourceKind kind = world.get<Resource>(w.target)->kind;
                        harvested.emit(ResourceHarvestedEvent{.resource_index = w.target.index,
                                                              .resource_generation = w.target.generation,
                                                              .kind = static_cast<std::uint32_t>(kind), .amount = 1});
                        world.emplace<Carrying>(e, kind); // Carrying не в этой выборке: добавлять можно
                        w.state = State::ToStockpile;
                        w.target = ECS::Entity{};
                        p.goal = tile_center({static_cast<int>(stockpile_tiles.position.x) + slot_x(rng),
                                              static_cast<int>(stockpile_tiles.position.y) + slot_y(rng)});
                    }
                    break;
                case State::ToStockpile:
                    if (arrived) {
                        const Carrying* load = world.get<Carrying>(e);
                        delivered.emit(ItemDeliveredEvent{.kind = static_cast<std::uint32_t>(load->kind), .amount = 1});
                        world.remove<Carrying>(e);
                        w.state = State::Idle;
                    }
                    break;
                case State::Idle: break;
            }

            // Клип зависит от состояния; при смене клипа анимация начинается сначала.
            const ClipId clip = w.state == State::Working ? work : (w.state == State::Idle ? idle : walk);
            if (anim.state.clip != clip) anim.state = AnimationState::start(clip);
            advance_animations(std::span(&anim.state, 1), anims, dt);
        });
    }
};

/// Chronicle — история: видит только события, не данные модулей.
struct Chronicle {
    es::EventReader<MilestoneEvent> milestones;
    es::EventReader<ResourceDepletedEvent> depleted;
    es::EventReader<ResourceSpawnedEvent> spawned;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Chronicle")
                                    .consumes<MilestoneEvent>()
                                    .consumes<ResourceDepletedEvent>()
                                    .consumes<ResourceSpawnedEvent>();
        milestones = bus.reader<MilestoneEvent>(id);
        depleted = bus.reader<ResourceDepletedEvent>(id);
        spawned = bus.reader<ResourceSpawnedEvent>(id);
    }

    void founding(const ECS::World& world) const {
        int trees = 0;
        int rocks = 0;
        if (const auto* pool = world.find_pool<Resource>()) {
            for (const Resource& r : pool->components()) (r.kind == ResourceKind::Tree ? trees : rocks)++;
        }
        std::println("[{}] Chronicle: {} settlers founded a colony among {} trees and {} rocks", game_time(0),
                     world.count<Worker>(), trees, rocks);
    }

    void tick(es::Tick now) {
        for (const MilestoneEvent& m : milestones.events()) {
            std::println("[{}] Chronicle: the stockpile now holds {} {}", game_time(now), m.total,
                         kind_name(static_cast<ResourceKind>(m.kind)));
        }
        for (const ResourceDepletedEvent& d : depleted.events()) {
            std::println("[{}] Chronicle: {} at ({}, {}) is gone", game_time(now),
                         static_cast<ResourceKind>(d.kind) == ResourceKind::Tree ? "a tree" : "a rock", d.tile_x, d.tile_y);
        }
        for (const ResourceSpawnedEvent& s : spawned.events()) {
            const bool tree = static_cast<ResourceKind>(s.kind) == ResourceKind::Tree;
            std::println("[{}] Chronicle: {} at ({}, {})", game_time(now),
                         s.regrown ? "a sapling sprouted" : (tree ? "a tree was planted" : "a rock was hauled in"),
                         s.tile_x, s.tile_y);
        }
    }
};

// =============================================================================
// Игра: мир ECS + модули, порядок вызова, отрисовка.
// =============================================================================

class Colony final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {map_w * tile, map_h * tile}; }

    void setup(Core::App& app) override {
        es::EventBus& bus = app.bus();
        orders.declare(bus);
        map.declare(bus);
        jobs.declare(bus);
        colonists.declare(bus);
        economy.declare(bus);
        chronicle.declare(bus);

        atlas = app.renderer().create_texture(make_atlas());
        map.generate(world);
        colonists.spawn(world);
        chronicle.founding(world);
    }

    void tick(Core::App& app) override {
        const es::Tick now = app.tick();
        orders.tick();
        map.tick(world, now);
        jobs.tick(world, economy, now);
        colonists.tick(world, app.tick_seconds());
        economy.tick();
        chronicle.tick(now);
    }

    void render(Core::App& /*app*/, Renderer2D& r) override {
        for (int y = 0; y < map_h; ++y) {
            for (int x = 0; x < map_w; ++x) {
                const glm::ivec2 t{x, y};
                const int cell = in_stockpile(t) ? static_cast<int>(StockpileFloor) : map.terrain[static_cast<std::size_t>(y * map_w + x)];
                r.draw(sprite(tile_center(t), cell, -10));
            }
        }

        // Содержимое склада: по предмету на тайл, сначала брёвна, потом камни.
        int slot = 0;
        const int slots = static_cast<int>(stockpile_tiles.size.x * stockpile_tiles.size.y);
        for (const auto& [kind, cell] : {std::pair{ResourceKind::Tree, LogItem}, std::pair{ResourceKind::Rock, StoneItem}}) {
            for (std::uint32_t i = 0; i < std::min<std::uint32_t>(economy.amount(kind), 12) && slot < slots; ++i, ++slot) {
                const glm::ivec2 t{static_cast<int>(stockpile_tiles.position.x) + slot % static_cast<int>(stockpile_tiles.size.x),
                                   static_cast<int>(stockpile_tiles.position.y) + slot / static_cast<int>(stockpile_tiles.size.x)};
                r.draw(sprite(tile_center(t), cell, -8));
            }
        }

        world.view<const Resource, const Tile>().each([&](const Resource& res, const Tile& t) {
            const glm::vec2 c = tile_center(t.at);
            r.draw(sprite(c, res.kind == ResourceKind::Tree ? TreeSprite : RockSprite, 0));
            if (res.amount < res.max_amount) {
                const float fraction = static_cast<float>(res.amount) / static_cast<float>(res.max_amount);
                r.fill_rect({c + glm::vec2{-7.0f, 7.0f}, {14.0f, 2.0f}}, Color{0, 0, 0, 160}, 1);
                r.fill_rect({c + glm::vec2{-7.0f, 7.0f}, {14.0f * fraction, 2.0f}}, Colors::yellow, 1);
            }
        });

        world.view<const Worker, const Position, const Anim>().each(
            [&](ECS::Entity e, const Worker& w, const Position& p, const Anim& anim) {
                SpriteInstance s = sprite(p.now, Idle, 2);
                s.uv = current_uv(anim.state, colonists.anims);
                s.flip = w.facing_left ? SpriteFlip::X : SpriteFlip::None;
                r.draw(s);
                if (const Carrying* load = world.get<Carrying>(e)) {
                    r.draw(sprite(p.now + glm::vec2{0.0f, -11.0f}, load->kind == ResourceKind::Tree ? LogItem : StoneItem, 3));
                }
                if (orders.show_jobs && w.state != State::Idle) r.draw_line(p.now, p.goal, 1.0f, Color{255, 255, 255, 90}, 4);
            });
    }

    void render_overlay(Core::App& /*app*/, Renderer2D& r) override {
        // Запасы склада: коричневая полоса — брёвна, серая — камень, засечка каждые 10 штук.
        const Color colors[] = {Color::from_rgba(0x8B5A2BFF), Color::from_rgba(0x9A9AA3FF)};
        for (std::size_t k = 0; k < 2; ++k) {
            const float y = 12.0f + static_cast<float>(k) * 16.0f;
            const std::uint32_t amount = economy.stock[k];
            r.fill_rect({{12.0f, y}, {static_cast<float>(amount) * 3.0f, 10.0f}}, colors[k], 1);
            for (std::uint32_t mark = 10; mark <= amount; mark += 10) {
                r.fill_rect({{12.0f + static_cast<float>(mark) * 3.0f - 1.0f, y - 2.0f}, {2.0f, 14.0f}}, Colors::white, 2);
            }
        }
    }

    [[nodiscard]] std::string status() const override {
        int working = 0;
        if (const auto* pool = world.find_pool<Worker>()) {
            for (const Worker& w : pool->components()) working += w.state == State::Working ? 1 : 0;
        }
        return std::format("logs {} | stone {} | resources {} | working {}", economy.stock[0], economy.stock[1],
                           world.count<Resource>(), working);
    }

    void shutdown(Core::App& app) override {
        std::println("\n===== Colony : summary after {} ticks ({}) =====", app.tick(), game_time(app.tick()));
        std::println("stockpile: {} logs, {} stones; jobs assigned {}", economy.stock[0], economy.stock[1], jobs.assigned);
        std::println("ECS: {} entities ({} resources, {} colonists), {} slots ever used", world.alive(),
                     world.count<Resource>(), world.count<Worker>(), world.registry().slots());
    }

private:
    SpriteInstance sprite(glm::vec2 center, int cell, std::int32_t layer) const {
        return SpriteInstance{.position = center, .size = {tile, tile}, .uv = atlas_uv(cell), .texture = atlas, .layer = layer};
    }

    ECS::World world;
    Orders orders;
    World map;
    Jobs jobs;
    Colonists colonists;
    Economy economy;
    Chronicle chronicle;
    TextureHandle atlas;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<Colony>({.title = "Colony", .ticks_per_second = 30.0}, argc, argv);
}
