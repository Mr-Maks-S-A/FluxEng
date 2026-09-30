/**
 * @file main.cpp
 * @brief Colony — упрощённый RimWorld: колонисты рубят деревья, добывают камень и носят всё на склад.
 *
 * Модули общаются только событиями; общие данные (позиции, ресурсы) читаются напрямую —
 * в движке это будут компоненты ECS. Порядок модулей в тике не важен для корректности:
 * всё, что отправлено в тике N, видно в N+1.
 *
 *   Orders ──order_place──▶ World ──resource_spawned──▶ Chronicle
 *                            │  ▲
 *              resource_depleted  resource_harvested
 *                            ▼  │
 *   Jobs ──job_assigned──▶ Colonists ──item_delivered──▶ Economy ──milestone──▶ Chronicle
 *
 * Управление: ЛКМ — посадить дерево, ПКМ — положить камень, J — линии заданий.
 * Общие клавиши — см. Core::App.
 */

#include <Core/Core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <print>
#include <random>
#include <string_view>
#include <vector>

namespace es = EventSystem;
using namespace RendererSystem;

namespace {

// =============================================================================
// Контракт событий
// =============================================================================

enum class ResourceKind : std::uint32_t { Tree = 0, Rock = 1 };

struct PlaceOrderEvent {
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    std::uint32_t kind = 0;

    static constexpr std::string_view event_name = "colony.order_place";
    using fields = es::Fields<es::Field<"tile_x", &PlaceOrderEvent::tile_x>, es::Field<"tile_y", &PlaceOrderEvent::tile_y>,
                              es::Field<"kind", &PlaceOrderEvent::kind>>;
};

struct ResourceSpawnedEvent {
    std::uint32_t resource = 0;
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    std::uint32_t kind = 0;
    std::uint32_t regrown = 0; ///< 1 — выросло само, 0 — по приказу игрока.

    static constexpr std::string_view event_name = "colony.resource_spawned";
    using fields = es::Fields<es::Field<"resource", &ResourceSpawnedEvent::resource>,
                              es::Field<"tile_x", &ResourceSpawnedEvent::tile_x>,
                              es::Field<"tile_y", &ResourceSpawnedEvent::tile_y>,
                              es::Field<"kind", &ResourceSpawnedEvent::kind>,
                              es::Field<"regrown", &ResourceSpawnedEvent::regrown>>;
};

struct JobAssignedEvent {
    std::uint32_t colonist = 0;
    std::uint32_t resource = 0;

    static constexpr std::string_view event_name = "colony.job_assigned";
    using fields = es::Fields<es::Field<"colonist", &JobAssignedEvent::colonist>,
                              es::Field<"resource", &JobAssignedEvent::resource>>;
};

struct ResourceHarvestedEvent {
    std::uint32_t colonist = 0;
    std::uint32_t resource = 0;
    std::uint32_t kind = 0;
    std::uint32_t amount = 0;

    static constexpr std::string_view event_name = "colony.resource_harvested";
    using fields = es::Fields<es::Field<"colonist", &ResourceHarvestedEvent::colonist>,
                              es::Field<"resource", &ResourceHarvestedEvent::resource>,
                              es::Field<"kind", &ResourceHarvestedEvent::kind>,
                              es::Field<"amount", &ResourceHarvestedEvent::amount>>;
};

struct ResourceDepletedEvent {
    std::uint32_t resource = 0;
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    std::uint32_t kind = 0;

    static constexpr std::string_view event_name = "colony.resource_depleted";
    using fields = es::Fields<es::Field<"resource", &ResourceDepletedEvent::resource>,
                              es::Field<"tile_x", &ResourceDepletedEvent::tile_x>,
                              es::Field<"tile_y", &ResourceDepletedEvent::tile_y>,
                              es::Field<"kind", &ResourceDepletedEvent::kind>>;
};

struct ItemDeliveredEvent {
    std::uint32_t colonist = 0;
    std::uint32_t kind = 0;
    std::uint32_t amount = 0;

    static constexpr std::string_view event_name = "colony.item_delivered";
    using fields = es::Fields<es::Field<"colonist", &ItemDeliveredEvent::colonist>,
                              es::Field<"kind", &ItemDeliveredEvent::kind>,
                              es::Field<"amount", &ItemDeliveredEvent::amount>>;
};

struct MilestoneEvent {
    std::uint32_t kind = 0;
    std::uint32_t total = 0;

    static constexpr std::string_view event_name = "colony.milestone";
    using fields = es::Fields<es::Field<"kind", &MilestoneEvent::kind>, es::Field<"total", &MilestoneEvent::total>>;
};

// =============================================================================
// Мир
// =============================================================================

constexpr int map_w = 48;
constexpr int map_h = 30;
constexpr float tile = 16.0f;
constexpr Rect stockpile_tiles{{3.0f, 3.0f}, {6.0f, 4.0f}}; // в тайлах
constexpr int colonist_count = 10;
constexpr float colonist_speed = 48.0f; // пикселей в игровую секунду

/// Атлас 8×2 клеток по 16 px: всё рисуется одной-двумя текстурами.
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
    // make_grid_frames возвращает vector — в цикле отрисовки это аллокация на каждый спрайт,
    // поэтому UV клеток считаются один раз (в RendererSystem не хватает SpriteSheetGrid::cell_uv()).
    static const std::vector<AnimationFrame> cells = make_grid_frames({.columns = 8, .rows = 2}, 0, 16, 1.0f);
    return cells[static_cast<std::size_t>(index)].uv;
}

glm::vec2 tile_center(glm::ivec2 t) {
    return {(static_cast<float>(t.x) + 0.5f) * tile, (static_cast<float>(t.y) + 0.5f) * tile};
}

bool in_stockpile(glm::ivec2 t) {
    return stockpile_tiles.contains({static_cast<float>(t.x) + 0.5f, static_cast<float>(t.y) + 0.5f});
}

std::string game_time(es::Tick tick) {
    // Один тик — одна игровая минута.
    return std::format("day {}, {:02}:{:02}", tick / 1440 + 1, (tick / 60) % 24, tick % 60);
}

// =============================================================================

class Colony final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {map_w * tile, map_h * tile}; }

    void setup(Core::App& app) override {
        es::EventBus& bus = app.bus();

        const es::ModuleId orders = bus.declare_module("Orders")
                                        .consumes<Core::KeyEvent>()
                                        .consumes<Core::MouseButtonEvent>()
                                        .produces<PlaceOrderEvent>();
        const es::ModuleId world = bus.declare_module("World")
                                       .consumes<PlaceOrderEvent>()
                                       .consumes<ResourceHarvestedEvent>()
                                       .produces<ResourceSpawnedEvent>()
                                       .produces<ResourceDepletedEvent>();
        const es::ModuleId jobs = bus.declare_module("Jobs")
                                      .consumes<ResourceHarvestedEvent>()
                                      .consumes<ResourceDepletedEvent>()
                                      .produces<JobAssignedEvent>();
        const es::ModuleId colonists = bus.declare_module("Colonists")
                                           .consumes<JobAssignedEvent>()
                                           .consumes<ResourceDepletedEvent>()
                                           .produces<ResourceHarvestedEvent>()
                                           .produces<ItemDeliveredEvent>();
        const es::ModuleId economy = bus.declare_module("Economy").consumes<ItemDeliveredEvent>().produces<MilestoneEvent>();
        const es::ModuleId chronicle = bus.declare_module("Chronicle")
                                           .consumes<MilestoneEvent>()
                                           .consumes<ResourceDepletedEvent>()
                                           .consumes<ResourceSpawnedEvent>();

        m_orders_keys = bus.reader<Core::KeyEvent>(orders);
        m_orders_mouse = bus.reader<Core::MouseButtonEvent>(orders);
        m_orders_out = bus.writer<PlaceOrderEvent>(orders);
        m_world_orders = bus.reader<PlaceOrderEvent>(world);
        m_world_harvested = bus.reader<ResourceHarvestedEvent>(world);
        m_world_spawned = bus.writer<ResourceSpawnedEvent>(world);
        m_world_depleted = bus.writer<ResourceDepletedEvent>(world);
        m_jobs_harvested = bus.reader<ResourceHarvestedEvent>(jobs);
        m_jobs_depleted = bus.reader<ResourceDepletedEvent>(jobs);
        m_jobs_out = bus.writer<JobAssignedEvent>(jobs);
        m_colonists_jobs = bus.reader<JobAssignedEvent>(colonists);
        m_colonists_depleted = bus.reader<ResourceDepletedEvent>(colonists);
        m_colonists_harvested = bus.writer<ResourceHarvestedEvent>(colonists);
        m_colonists_delivered = bus.writer<ItemDeliveredEvent>(colonists);
        m_economy_delivered = bus.reader<ItemDeliveredEvent>(economy);
        m_economy_milestones = bus.writer<MilestoneEvent>(economy);
        m_chronicle_milestones = bus.reader<MilestoneEvent>(chronicle);
        m_chronicle_depleted = bus.reader<ResourceDepletedEvent>(chronicle);
        m_chronicle_spawned = bus.reader<ResourceSpawnedEvent>(chronicle);

        m_atlas = app.renderer().create_texture(make_atlas());
        m_walk = m_anims.add({.name = "colonist.walk", .frames = make_grid_frames({.columns = 8, .rows = 2}, WalkFirst, 4, 0.12f)});
        m_work = m_anims.add({.name = "colonist.work", .frames = make_grid_frames({.columns = 8, .rows = 2}, WorkFirst, 2, 0.2f)});
        m_idle = m_anims.add({.name = "colonist.idle", .frames = make_grid_frames({.columns = 8, .rows = 2}, Idle, 1, 1.0f)});

        generate_world();
    }

    void tick(Core::App& app) override {
        const es::Tick now = app.tick();
        tick_orders();
        tick_world(now);
        tick_jobs();
        tick_colonists(app.tick_seconds());
        tick_economy();
        tick_chronicle(now);
    }

    void render(Core::App& /*app*/, Renderer2D& r) override {
        for (int y = 0; y < map_h; ++y) {
            for (int x = 0; x < map_w; ++x) {
                const glm::ivec2 t{x, y};
                const int cell = in_stockpile(t) ? StockpileFloor : m_terrain[static_cast<std::size_t>(y * map_w + x)];
                r.draw(sprite(tile_center(t), cell, -10));
            }
        }

        // Содержимое склада: по предмету на тайл, сначала брёвна, потом камни.
        int slot = 0;
        const int slots = static_cast<int>(stockpile_tiles.size.x * stockpile_tiles.size.y);
        for (const auto& [kind, cell] : {std::pair{ResourceKind::Tree, LogItem}, std::pair{ResourceKind::Rock, StoneItem}}) {
            for (std::uint32_t i = 0; i < std::min<std::uint32_t>(m_stock[static_cast<std::size_t>(kind)], 12) && slot < slots; ++i, ++slot) {
                const glm::ivec2 t{static_cast<int>(stockpile_tiles.position.x) + slot % static_cast<int>(stockpile_tiles.size.x),
                                   static_cast<int>(stockpile_tiles.position.y) + slot / static_cast<int>(stockpile_tiles.size.x)};
                r.draw(sprite(tile_center(t), cell, -8));
            }
        }

        for (const Resource& res : m_resources) {
            if (!res.alive) continue;
            const glm::vec2 c = tile_center(res.tile);
            r.draw(sprite(c, res.kind == ResourceKind::Tree ? TreeSprite : RockSprite, 0));
            if (res.amount < res.max_amount) {
                const float fraction = static_cast<float>(res.amount) / static_cast<float>(res.max_amount);
                r.fill_rect({c + glm::vec2{-7.0f, 7.0f}, {14.0f, 2.0f}}, Color{0, 0, 0, 160}, 1);
                r.fill_rect({c + glm::vec2{-7.0f, 7.0f}, {14.0f * fraction, 2.0f}}, Colors::yellow, 1);
            }
        }

        for (std::size_t i = 0; i < m_pos.size(); ++i) {
            SpriteInstance s = sprite(m_pos[i], Idle, 2);
            s.uv = current_uv(m_anim[i], m_anims);
            s.flip = m_facing_left[i] ? SpriteFlip::X : SpriteFlip::None;
            r.draw(s);
            if (m_state[i] == State::ToStockpile) {
                r.draw(sprite(m_pos[i] + glm::vec2{0.0f, -11.0f}, m_carrying[i] == ResourceKind::Tree ? LogItem : StoneItem, 3));
            }
            if (m_show_jobs && m_state[i] != State::Idle) {
                r.draw_line(m_pos[i], m_goal[i], 1.0f, Color{255, 255, 255, 90}, 4);
            }
        }
    }

    void render_overlay(Core::App& /*app*/, Renderer2D& r) override {
        // Запасы склада: коричневая полоса — брёвна, серая — камень, засечка каждые 10 штук.
        const Color colors[] = {Color::from_rgba(0x8B5A2BFF), Color::from_rgba(0x9A9AA3FF)};
        for (std::size_t k = 0; k < 2; ++k) {
            const float y = 12.0f + static_cast<float>(k) * 16.0f;
            r.fill_rect({{12.0f, y}, {static_cast<float>(m_stock[k]) * 3.0f, 10.0f}}, colors[k], 1);
            for (std::uint32_t mark = 10; mark <= m_stock[k]; mark += 10) {
                r.fill_rect({{12.0f + static_cast<float>(mark) * 3.0f - 1.0f, y - 2.0f}, {2.0f, 14.0f}}, Colors::white, 2);
            }
        }
    }

    [[nodiscard]] std::string status() const override {
        const auto alive = std::ranges::count_if(m_resources, [](const Resource& r) { return r.alive; });
        return std::format("logs {} | stone {} | resources {} | working {}", m_stock[0], m_stock[1], alive,
                           std::ranges::count(m_state, State::Working));
    }

private:
    enum class State : std::uint8_t { Idle, ToResource, Working, ToStockpile };

    struct Resource {
        glm::ivec2 tile;
        ResourceKind kind;
        int amount;
        int max_amount;
        bool alive;
    };

    struct Regrowth {
        es::Tick due;
        glm::ivec2 near;
    };

    SpriteInstance sprite(glm::vec2 center, int cell, std::int32_t layer) const {
        return SpriteInstance{.position = center, .size = {tile, tile}, .uv = atlas_uv(cell), .texture = m_atlas, .layer = layer};
    }

    // ------------------------------------------------------------------ генерация

    void generate_world() {
        m_terrain.resize(map_w * map_h);
        for (int& cell : m_terrain) cell = std::uniform_int_distribution<int>(0, 9)(m_rng) == 0 ? Dirt : Grass;

        std::uniform_int_distribution<int> x(0, map_w - 1);
        std::uniform_int_distribution<int> y(0, map_h - 1);
        for (int i = 0; i < 40; ++i) spawn({x(m_rng), y(m_rng)}, i < 28 ? ResourceKind::Tree : ResourceKind::Rock, false);

        std::uniform_real_distribution<float> offset(-20.0f, 20.0f);
        const glm::vec2 home = (stockpile_tiles.center()) * tile;
        for (int i = 0; i < colonist_count; ++i) {
            m_pos.push_back(home + glm::vec2{offset(m_rng), offset(m_rng)});
            m_goal.push_back(m_pos.back());
            m_state.push_back(State::Idle);
            m_target.push_back(0);
            m_work_timer.push_back(0);
            m_carrying.push_back(ResourceKind::Tree);
            m_facing_left.push_back(false);
            m_awaiting_job.push_back(false);
            m_anim.push_back(AnimationState::start(m_idle));
        }
        std::println("[{}] Chronicle: {} settlers founded a colony among {} trees and {} rocks", game_time(0),
                     colonist_count, std::ranges::count(m_resources, ResourceKind::Tree, &Resource::kind),
                     std::ranges::count(m_resources, ResourceKind::Rock, &Resource::kind));
    }

    /// Создаёт ресурс; возвращает индекс или -1, если тайл занят.
    /// Индексы не переиспользуются: без ID с поколением старые события указали бы на новый ресурс.
    std::int64_t spawn(glm::ivec2 t, ResourceKind kind, bool announce, bool regrown = false) {
        if (t.x < 0 || t.y < 0 || t.x >= map_w || t.y >= map_h || in_stockpile(t)) return -1;
        for (const Resource& r : m_resources) {
            if (r.alive && r.tile == t) return -1;
        }
        const int amount = kind == ResourceKind::Tree ? 3 : 5;
        m_resources.push_back(Resource{.tile = t, .kind = kind, .amount = amount, .max_amount = amount, .alive = true});
        m_reserved_by.push_back(-1);
        const auto index = static_cast<std::uint32_t>(m_resources.size() - 1);
        if (announce) {
            m_world_spawned.emit(ResourceSpawnedEvent{.resource = index, .tile_x = t.x, .tile_y = t.y,
                                                      .kind = static_cast<std::uint32_t>(kind), .regrown = regrown ? 1u : 0u});
        }
        return index;
    }

    // ------------------------------------------------------------------ Orders

    void tick_orders() {
        for (const Core::KeyEvent& key : m_orders_keys.events()) {
            if (key.action == GLFW_PRESS && key.key == GLFW_KEY_J) m_show_jobs = !m_show_jobs;
        }
        for (const Core::MouseButtonEvent& click : m_orders_mouse.events()) {
            if (click.action != GLFW_PRESS || click.button == GLFW_MOUSE_BUTTON_MIDDLE) continue;
            m_orders_out.emit(PlaceOrderEvent{
                .tile_x = static_cast<std::int32_t>(std::floor(click.world_x / tile)),
                .tile_y = static_cast<std::int32_t>(std::floor(click.world_y / tile)),
                .kind = static_cast<std::uint32_t>(click.button == GLFW_MOUSE_BUTTON_LEFT ? ResourceKind::Tree : ResourceKind::Rock),
            });
        }
    }

    // ------------------------------------------------------------------ World

    void tick_world(es::Tick now) {
        for (const PlaceOrderEvent& order : m_world_orders.events()) {
            spawn({order.tile_x, order.tile_y}, static_cast<ResourceKind>(order.kind), true);
        }
        for (const ResourceHarvestedEvent& h : m_world_harvested.events()) {
            Resource& res = m_resources[h.resource];
            if (!res.alive) continue;
            res.amount -= static_cast<int>(h.amount);
            if (res.amount <= 0) {
                res.alive = false;
                m_world_depleted.emit(ResourceDepletedEvent{.resource = h.resource, .tile_x = res.tile.x,
                                                            .tile_y = res.tile.y, .kind = h.kind});
                if (res.kind == ResourceKind::Tree) {
                    // Отложенных событий в шине нет — очередь отрастания ведётся вручную.
                    m_regrowth.push_back(Regrowth{.due = now + 900, .near = res.tile});
                }
            }
        }
        std::uniform_int_distribution<int> jitter(-3, 3);
        std::erase_if(m_regrowth, [&](const Regrowth& g) {
            if (g.due > now) return false;
            spawn(g.near + glm::ivec2{jitter(m_rng), jitter(m_rng)}, ResourceKind::Tree, true, true);
            return true; // занятый тайл — саженец просто не вырос
        });
    }

    // ------------------------------------------------------------------ Jobs

    void tick_jobs() {
        for (const ResourceHarvestedEvent& h : m_jobs_harvested.events()) m_reserved_by[h.resource] = -1;
        for (const ResourceDepletedEvent& d : m_jobs_depleted.events()) m_reserved_by[d.resource] = -1;

        // Нужнее тот ресурс, которого на складе меньше.
        const ResourceKind wanted = m_stock[0] <= m_stock[1] ? ResourceKind::Tree : ResourceKind::Rock;
        for (std::size_t c = 0; c < m_pos.size(); ++c) {
            if (m_state[c] != State::Idle) {
                m_awaiting_job[c] = false;
                continue;
            }
            if (m_awaiting_job[c]) continue; // задание уже отправлено, Colonists получит его в следующем тике

            std::int64_t best = -1;
            float best_score = std::numeric_limits<float>::max();
            for (std::size_t r = 0; r < m_resources.size(); ++r) {
                if (!m_resources[r].alive || m_reserved_by[r] >= 0) continue;
                const glm::vec2 d = tile_center(m_resources[r].tile) - m_pos[c];
                const float score = std::sqrt(d.x * d.x + d.y * d.y) * (m_resources[r].kind == wanted ? 1.0f : 2.5f);
                if (score < best_score) {
                    best_score = score;
                    best = static_cast<std::int64_t>(r);
                }
            }
            if (best >= 0) {
                m_reserved_by[static_cast<std::size_t>(best)] = static_cast<std::int32_t>(c);
                m_jobs_out.emit(JobAssignedEvent{.colonist = static_cast<std::uint32_t>(c), .resource = static_cast<std::uint32_t>(best)});
                m_awaiting_job[c] = true;
            }
        }
    }

    // ------------------------------------------------------------------ Colonists

    void tick_colonists(float dt) {
        for (const JobAssignedEvent& job : m_colonists_jobs.events()) {
            if (m_state[job.colonist] != State::Idle || !m_resources[job.resource].alive) continue;
            m_state[job.colonist] = State::ToResource;
            m_target[job.colonist] = job.resource;
            m_goal[job.colonist] = tile_center(m_resources[job.resource].tile) + glm::vec2{-10.0f, 2.0f};
        }
        for (const ResourceDepletedEvent& d : m_colonists_depleted.events()) {
            for (std::size_t c = 0; c < m_pos.size(); ++c) {
                if (m_target[c] == d.resource && (m_state[c] == State::ToResource || m_state[c] == State::Working)) {
                    m_state[c] = State::Idle; // кто-то успел раньше
                }
            }
        }

        std::uniform_int_distribution<int> slot_x(0, static_cast<int>(stockpile_tiles.size.x) - 1);
        std::uniform_int_distribution<int> slot_y(0, static_cast<int>(stockpile_tiles.size.y) - 1);
        for (std::size_t c = 0; c < m_pos.size(); ++c) {
            const bool moving = m_state[c] == State::ToResource || m_state[c] == State::ToStockpile;
            bool arrived = false;
            if (moving) {
                const glm::vec2 d = m_goal[c] - m_pos[c];
                const float distance = std::sqrt(d.x * d.x + d.y * d.y);
                const float step = colonist_speed * dt;
                if (distance <= step) {
                    m_pos[c] = m_goal[c];
                    arrived = true;
                } else {
                    m_pos[c] += d / distance * step;
                    m_facing_left[c] = d.x < 0.0f;
                }
            }

            switch (m_state[c]) {
                case State::ToResource:
                    if (arrived) {
                        m_state[c] = State::Working;
                        m_facing_left[c] = false;
                        m_work_timer[c] = m_resources[m_target[c]].kind == ResourceKind::Tree ? 45 : 70;
                    }
                    break;
                case State::Working:
                    if (--m_work_timer[c] <= 0) {
                        const Resource& res = m_resources[m_target[c]];
                        m_colonists_harvested.emit(ResourceHarvestedEvent{.colonist = static_cast<std::uint32_t>(c),
                                                                          .resource = m_target[c],
                                                                          .kind = static_cast<std::uint32_t>(res.kind),
                                                                          .amount = 1});
                        m_carrying[c] = res.kind;
                        m_state[c] = State::ToStockpile;
                        m_goal[c] = tile_center({static_cast<int>(stockpile_tiles.position.x) + slot_x(m_rng),
                                                 static_cast<int>(stockpile_tiles.position.y) + slot_y(m_rng)});
                    }
                    break;
                case State::ToStockpile:
                    if (arrived) {
                        m_colonists_delivered.emit(ItemDeliveredEvent{.colonist = static_cast<std::uint32_t>(c),
                                                                      .kind = static_cast<std::uint32_t>(m_carrying[c]),
                                                                      .amount = 1});
                        m_state[c] = State::Idle;
                    }
                    break;
                case State::Idle: break;
            }

            // Клип зависит от состояния; при смене клипа анимация начинается сначала.
            const ClipId clip = m_state[c] == State::Working ? m_work : (m_state[c] == State::Idle ? m_idle : m_walk);
            if (m_anim[c].clip != clip) m_anim[c] = AnimationState::start(clip);
        }

        // Все анимации колонистов — один вызов по плотному массиву.
        advance_animations(m_anim, m_anims, dt);
    }

    // ------------------------------------------------------------------ Economy

    void tick_economy() {
        for (const ItemDeliveredEvent& item : m_economy_delivered.events()) {
            std::uint32_t& stock = m_stock[item.kind];
            const std::uint32_t before = stock;
            stock += item.amount;
            if (stock / 10 != before / 10) {
                m_economy_milestones.emit(MilestoneEvent{.kind = item.kind, .total = stock});
            }
        }
    }

    // ------------------------------------------------------------------ Chronicle

    void tick_chronicle(es::Tick now) {
        // Хроника — зачаток генерируемой истории: она видит только события, не данные модулей.
        for (const MilestoneEvent& m : m_chronicle_milestones.events()) {
            std::println("[{}] Chronicle: the stockpile now holds {} {}", game_time(now), m.total,
                         m.kind == 0 ? "logs" : "stones");
        }
        for (const ResourceDepletedEvent& d : m_chronicle_depleted.events()) {
            std::println("[{}] Chronicle: {} at ({}, {}) is gone", game_time(now),
                         d.kind == 0 ? "a tree" : "a rock", d.tile_x, d.tile_y);
        }
        for (const ResourceSpawnedEvent& s : m_chronicle_spawned.events()) {
            std::println("[{}] Chronicle: {} at ({}, {})", game_time(now),
                         s.regrown ? "a sapling sprouted" : (s.kind == 0 ? "a tree was planted" : "a rock was hauled in"),
                         s.tile_x, s.tile_y);
        }
    }

    // ---- писатели и читатели
    es::EventReader<Core::KeyEvent> m_orders_keys;
    es::EventReader<Core::MouseButtonEvent> m_orders_mouse;
    es::EventWriter<PlaceOrderEvent> m_orders_out;
    es::EventReader<PlaceOrderEvent> m_world_orders;
    es::EventReader<ResourceHarvestedEvent> m_world_harvested;
    es::EventWriter<ResourceSpawnedEvent> m_world_spawned;
    es::EventWriter<ResourceDepletedEvent> m_world_depleted;
    es::EventReader<ResourceHarvestedEvent> m_jobs_harvested;
    es::EventReader<ResourceDepletedEvent> m_jobs_depleted;
    es::EventWriter<JobAssignedEvent> m_jobs_out;
    es::EventReader<JobAssignedEvent> m_colonists_jobs;
    es::EventReader<ResourceDepletedEvent> m_colonists_depleted;
    es::EventWriter<ResourceHarvestedEvent> m_colonists_harvested;
    es::EventWriter<ItemDeliveredEvent> m_colonists_delivered;
    es::EventReader<ItemDeliveredEvent> m_economy_delivered;
    es::EventWriter<MilestoneEvent> m_economy_milestones;
    es::EventReader<MilestoneEvent> m_chronicle_milestones;
    es::EventReader<ResourceDepletedEvent> m_chronicle_depleted;
    es::EventReader<ResourceSpawnedEvent> m_chronicle_spawned;

    // ---- World
    std::vector<int> m_terrain;
    std::vector<Resource> m_resources;
    std::vector<Regrowth> m_regrowth;

    // ---- Jobs
    std::vector<std::int32_t> m_reserved_by;
    std::vector<bool> m_awaiting_job;

    // ---- Colonists: SoA, как будущие компоненты ECS
    std::vector<glm::vec2> m_pos;
    std::vector<glm::vec2> m_goal;
    std::vector<State> m_state;
    std::vector<std::uint32_t> m_target;
    std::vector<int> m_work_timer;
    std::vector<ResourceKind> m_carrying;
    std::vector<bool> m_facing_left;
    std::vector<AnimationState> m_anim;

    // ---- Economy
    std::array<std::uint32_t, 2> m_stock{};

    // ---- рендер
    TextureHandle m_atlas;
    AnimationLibrary m_anims;
    ClipId m_walk;
    ClipId m_work;
    ClipId m_idle;
    bool m_show_jobs = true;

    std::mt19937 m_rng{2024};
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<Colony>({.title = "Colony", .ticks_per_second = 30.0}, argc, argv);
}
