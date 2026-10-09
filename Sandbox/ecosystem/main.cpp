/**
 * @file main.cpp
 * @brief Ecosystem — трава, зайцы и лисы на ECS: популяции, которые рождаются и умирают.
 *
 * Что показывает:
 * - ECS::World вместо самодельных SoA-массивов: зайцы и лисы — сущности с компонентами
 *   Position, Heading, Energy и тегом вида;
 * - ссылки с поколением: лиса отправляет `eco.hunt` в тике N, Life разбирает его в тике N+1 —
 *   за это время заяц мог умереть, а его слот достаться новорождённому. `world.valid()` отбрасывает
 *   такие «устаревшие охоты» (счётчик stale в заголовке);
 * - единственный владелец структуры мира: создаёт и уничтожает сущности только Life,
 *   остальные модули меняют лишь значения компонентов — это безопасно во время обхода выборок;
 * - модули-структуры: свои порты, данные и declare(); мир и чужие модули приходят в tick() параметрами;
 * - интерполяция отрисовки между тиками (App::tick_alpha).
 *
 *   Spawner ──┐
 *   Rabbits ──┼─ eco.birth_request ─▶ Life ──▶ eco.born ──▶ Census
 *   Foxes ────┤   eco.hunt, eco.starved ─▶ Life ──▶ eco.died ──▶ Foxes (кормёжка), Census
 *   Migration ┘                          Rabbits ── eco.grazed ──▶ Grass
 *
 * Управление: ЛКМ — выпустить зайцев, ПКМ — выпустить лис. Общие клавиши — см. Core::App.
 */

#include <Core/Core.hpp>
#include <ECSSystem/ECSSystem.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <print>
#include <random>
#include <string_view>
#include <vector>

using InputSystem::Key;
using InputSystem::MouseButton;

namespace es = EventSystem;
using namespace RendererSystem;

namespace {

// =============================================================================
// Мир и правила
// =============================================================================

constexpr int grid_w = 160;
constexpr int grid_h = 90;
constexpr float cell_size = 6.0f;
constexpr std::uint8_t grass_max = 4;
constexpr int grass_regrow_per_tick = 100; ///< Сколько случайных клеток за тик подрастают на 1 (≈ ёмкость пастбища).

/// Параметры вида. Энергия — «сытость»: тратится каждый тик, пополняется едой, тратится на потомство.
struct SpeciesRules {
    float speed;          ///< Клеток за тик.
    float metabolism;     ///< Трата энергии за тик.
    float food_gain;      ///< Энергия за одну еду.
    float max_energy;
    float breed_at;       ///< Порог размножения.
    float breed_cost;     ///< Сколько энергии уходит на детёныша.
    float newborn_energy;
    std::size_t cap;      ///< Предел популяции: Life отклоняет рождения сверх него.
};

// Подобрано прогонами --ticks 4500: популяции колеблются (Лотка–Вольтерра), лисы отстают по фазе,
// ни один вид не вымирает. Пределы cap — страховка, в устойчивом режиме до них не доходит.
constexpr SpeciesRules rabbit_rules{.speed = 0.22f, .metabolism = 0.012f, .food_gain = 0.035f, .max_energy = 2.0f,
                                    .breed_at = 1.7f, .breed_cost = 0.9f, .newborn_energy = 0.6f, .cap = 1500};
constexpr SpeciesRules fox_rules{.speed = 0.23f, .metabolism = 0.005f, .food_gain = 0.6f, .max_energy = 3.0f,
                                 .breed_at = 2.4f, .breed_cost = 1.2f, .newborn_energy = 0.8f, .cap = 200};

constexpr float fox_sight = 6.0f;   ///< Клеток: дальше лиса зайца не видит.
constexpr float fox_reach = 0.9f;   ///< Клеток: на таком расстоянии лиса нападает.
constexpr int fox_hunt_cooldown = 20;

enum class Species : std::uint32_t { Rabbit = 0, Fox = 1 };
enum class Cause : std::uint32_t { Starved = 0, Eaten = 1 };

std::size_t cell_index(int x, int y) { return static_cast<std::size_t>(y) * grid_w + static_cast<std::size_t>(x); }
glm::ivec2 cell_of(glm::vec2 p) {
    return {std::clamp(static_cast<int>(p.x), 0, grid_w - 1), std::clamp(static_cast<int>(p.y), 0, grid_h - 1)};
}

// =============================================================================
// Компоненты ECS. Нулевое значение каждого — осмысленное (ZII).
// =============================================================================

struct Position {
    glm::vec2 now{0.0f};  ///< Клетки мира.
    glm::vec2 prev{0.0f}; ///< Позиция в прошлом тике — для интерполяции отрисовки.
};
struct Heading {
    float radians = 0.0f;
};
struct Energy {
    float value = 0.0f; ///< 0 — голоден до смерти.
};
struct Rabbit {};       ///< Тег вида.
struct Fox {
    int cooldown = 0;   ///< Тиков до следующей атаки; 0 — может нападать.
};
struct Dying {};        ///< Отправлен eco.starved, Life уничтожит в следующем тике.

/// Сущность в событии передаётся двумя полями: события — плоские структуры.
ECS::Entity entity(std::uint32_t index, std::uint32_t generation) { return {index, generation}; }

// =============================================================================
// События
// =============================================================================

/// Заяц съел траву в клетке.
struct GrazedEvent {
    std::int32_t x = 0;
    std::int32_t y = 0;

    static constexpr std::string_view event_name = "eco.grazed";
    using fields = es::Fields<es::Field<"x", &GrazedEvent::x>, es::Field<"y", &GrazedEvent::y>>;
};

/// Просьба о рождении: сущность создаёт только Life.
struct BirthRequestEvent {
    std::uint32_t species = 0;
    float x = 0.0f;
    float y = 0.0f;
    float energy = 0.0f;

    static constexpr std::string_view event_name = "eco.birth_request";
    using fields = es::Fields<es::Field<"species", &BirthRequestEvent::species>, es::Field<"x", &BirthRequestEvent::x>,
                              es::Field<"y", &BirthRequestEvent::y>, es::Field<"energy", &BirthRequestEvent::energy>>;
};

/// Лиса поймала зайца. Засчитает ли это Life, зависит от того, живы ли оба к следующему тику.
struct HuntEvent {
    std::uint32_t predator_index = 0;
    std::uint32_t predator_generation = 0;
    std::uint32_t prey_index = 0;
    std::uint32_t prey_generation = 0;

    [[nodiscard]] ECS::Entity predator() const { return entity(predator_index, predator_generation); }
    [[nodiscard]] ECS::Entity prey() const { return entity(prey_index, prey_generation); }

    static constexpr std::string_view event_name = "eco.hunt";
    using fields = es::Fields<es::Field<"predator_index", &HuntEvent::predator_index>,
                              es::Field<"predator_generation", &HuntEvent::predator_generation>,
                              es::Field<"prey_index", &HuntEvent::prey_index>,
                              es::Field<"prey_generation", &HuntEvent::prey_generation>>;
};

/// Энергия кончилась.
struct StarvedEvent {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    static constexpr std::string_view event_name = "eco.starved";
    using fields = es::Fields<es::Field<"index", &StarvedEvent::index>, es::Field<"generation", &StarvedEvent::generation>>;
};

/// Существо родилось.
struct BornEvent {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    std::uint32_t species = 0;

    static constexpr std::string_view event_name = "eco.born";
    using fields = es::Fields<es::Field<"index", &BornEvent::index>, es::Field<"generation", &BornEvent::generation>,
                              es::Field<"species", &BornEvent::species>>;
};

/// Существо умерло (сущность уже уничтожена). Для съеденных — кто съел.
struct DiedEvent {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    std::uint32_t species = 0;
    std::uint32_t cause = 0;
    std::uint32_t killer_index = 0; ///< Entity{} — «убийцы нет».
    std::uint32_t killer_generation = 0;

    [[nodiscard]] ECS::Entity killer() const { return entity(killer_index, killer_generation); }

    static constexpr std::string_view event_name = "eco.died";
    using fields = es::Fields<es::Field<"index", &DiedEvent::index>, es::Field<"generation", &DiedEvent::generation>,
                              es::Field<"species", &DiedEvent::species>, es::Field<"cause", &DiedEvent::cause>,
                              es::Field<"killer_index", &DiedEvent::killer_index>,
                              es::Field<"killer_generation", &DiedEvent::killer_generation>>;
};

/// Шаг по направлению; от краёв мира отражаемся.
void step(Position& p, Heading& h, float speed) {
    p.prev = p.now;
    glm::vec2 next = p.now + glm::vec2{std::cos(h.radians), std::sin(h.radians)} * speed;
    if (next.x < 0.0f || next.x >= static_cast<float>(grid_w)) {
        h.radians = std::numbers::pi_v<float> - h.radians;
        next.x = std::clamp(next.x, 0.0f, static_cast<float>(grid_w) - 0.01f);
    }
    if (next.y < 0.0f || next.y >= static_cast<float>(grid_h)) {
        h.radians = -h.radians;
        next.y = std::clamp(next.y, 0.0f, static_cast<float>(grid_h) - 0.01f);
    }
    p.now = next;
}

// =============================================================================
// Модули. У каждого: порты (reader/writer), свои данные, declare() и tick().
// =============================================================================

/// Трава: съедается по событиям, отрастает сама. Данные — сетка, а не сущности.
struct Grass {
    es::EventReader<GrazedEvent> grazed;
    std::vector<std::uint8_t> level;
    std::mt19937 rng{101};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Grass").consumes<GrazedEvent>();
        grazed = bus.reader<GrazedEvent>(id);
    }

    void setup() {
        level.resize(static_cast<std::size_t>(grid_w) * grid_h);
        std::uniform_int_distribution<int> any(0, grass_max);
        for (std::uint8_t& l : level) l = static_cast<std::uint8_t>(any(rng));
    }

    [[nodiscard]] std::uint8_t at(glm::ivec2 c) const { return level[cell_index(c.x, c.y)]; }

    void tick() {
        for (const GrazedEvent& g : grazed.events()) {
            std::uint8_t& l = level[cell_index(g.x, g.y)];
            if (l > 0) --l;
        }
        std::uniform_int_distribution<std::size_t> any(0, level.size() - 1);
        for (int i = 0; i < grass_regrow_per_tick; ++i) {
            std::uint8_t& l = level[any(rng)];
            if (l < grass_max) ++l;
        }
    }

    void render(Renderer2D& r) const {
        static constexpr std::array<std::uint32_t, grass_max + 1> shades = {0x00000000, 0x2C4A22FF, 0x36612AFF,
                                                                            0x3F7A31FF, 0x4B9138FF};
        for (int y = 0; y < grid_h; ++y) {
            for (int x = 0; x < grid_w; ++x) {
                const std::uint8_t l = level[cell_index(x, y)];
                if (l == 0) continue;
                r.fill_rect({{static_cast<float>(x) * cell_size, static_cast<float>(y) * cell_size}, {cell_size, cell_size}},
                            Color::from_rgba(shades[l]), 0);
            }
        }
    }
};

/// Зайцы: бродят, тянутся к траве, едят, размножаются. Меняют только свои компоненты.
struct Rabbits {
    es::EventWriter<GrazedEvent> grazed;
    es::EventWriter<BirthRequestEvent> births;
    es::EventWriter<StarvedEvent> starved;
    std::mt19937 rng{202};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Rabbits")
                                    .produces<GrazedEvent>(es::ChannelConfig{.reserve = 2048, .max_events_per_tick = 4096})
                                    .produces<BirthRequestEvent>(es::ChannelConfig{.reserve = 512, .max_events_per_tick = 4096})
                                    .produces<StarvedEvent>(es::ChannelConfig{.reserve = 256, .max_events_per_tick = 4096});
        grazed = bus.writer<GrazedEvent>(id);
        births = bus.writer<BirthRequestEvent>(id);
        starved = bus.writer<StarvedEvent>(id);
    }

    void tick(ECS::World& world, const Grass& grass) {
        std::uniform_real_distribution<float> turn(-0.6f, 0.6f);
        std::uniform_real_distribution<float> jitter(-0.5f, 0.5f);
        world.view<const Rabbit, Position, Heading, Energy>().each(
            [&](ECS::Entity e, const Rabbit&, Position& p, Heading& h, Energy& energy) {
                if (world.has<Dying>(e)) {
                    p.prev = p.now;
                    return;
                }
                // Тянемся к самой густой траве среди соседних клеток, иначе бредём.
                const glm::ivec2 here = cell_of(p.now);
                glm::ivec2 best = here;
                for (const glm::ivec2 d : {glm::ivec2{1, 0}, glm::ivec2{-1, 0}, glm::ivec2{0, 1}, glm::ivec2{0, -1}}) {
                    const glm::ivec2 n = here + d;
                    if (n.x >= 0 && n.y >= 0 && n.x < grid_w && n.y < grid_h && grass.at(n) > grass.at(best)) best = n;
                }
                if (best != here) {
                    const glm::vec2 to = glm::vec2(best) + 0.5f + glm::vec2{jitter(rng), jitter(rng)} - p.now;
                    h.radians = std::atan2(to.y, to.x);
                } else {
                    h.radians += turn(rng);
                }
                step(p, h, rabbit_rules.speed);

                energy.value -= rabbit_rules.metabolism;
                const glm::ivec2 c = cell_of(p.now);
                if (grass.at(c) > 0 && energy.value < rabbit_rules.max_energy) {
                    grazed.emit(GrazedEvent{.x = c.x, .y = c.y});
                    energy.value += rabbit_rules.food_gain;
                }
                if (energy.value >= rabbit_rules.breed_at) {
                    energy.value -= rabbit_rules.breed_cost;
                    births.emit(BirthRequestEvent{.species = static_cast<std::uint32_t>(Species::Rabbit), .x = p.now.x,
                                                  .y = p.now.y, .energy = rabbit_rules.newborn_energy});
                }
                if (energy.value <= 0.0f) {
                    world.emplace<Dying>(e); // пул Dying ≠ ведущий пул выборки: добавлять можно
                    starved.emit(StarvedEvent{.index = e.index, .generation = e.generation});
                }
            });
    }
};

/// Лисы: ищут ближайшего зайца, догоняют, охотятся. Сытость — только за засчитанную охоту.
struct Foxes {
    es::EventReader<DiedEvent> died;
    es::EventWriter<HuntEvent> hunts;
    es::EventWriter<BirthRequestEvent> births;
    es::EventWriter<StarvedEvent> starved;
    std::mt19937 rng{303};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Foxes")
                                    .consumes<DiedEvent>()
                                    .produces<HuntEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 1024})
                                    .produces<BirthRequestEvent>()
                                    .produces<StarvedEvent>();
        died = bus.reader<DiedEvent>(id);
        hunts = bus.writer<HuntEvent>(id);
        births = bus.writer<BirthRequestEvent>(id);
        starved = bus.writer<StarvedEvent>(id);
    }

    void tick(ECS::World& world) {
        // Охота засчитана: кормим охотника, если он сам ещё жив (устаревшая ссылка вернёт nullptr).
        for (const DiedEvent& d : died.events()) {
            if (static_cast<Cause>(d.cause) != Cause::Eaten) continue;
            if (Energy* e = world.get<Energy>(d.killer())) e->value = std::min(e->value + fox_rules.food_gain, fox_rules.max_energy);
        }

        auto prey = world.view<const Rabbit, const Position>();
        std::uniform_real_distribution<float> turn(-0.4f, 0.4f);
        world.view<Fox, Position, Heading, Energy>().each([&](ECS::Entity me, Fox& fox, Position& p, Heading& h, Energy& energy) {
            if (world.has<Dying>(me)) {
                p.prev = p.now;
                return;
            }
            if (fox.cooldown > 0) --fox.cooldown;

            // Ближайший живой заяц в поле зрения.
            ECS::Entity target{};
            glm::vec2 target_at{0.0f};
            float best = fox_sight * fox_sight;
            prey.each([&](ECS::Entity rabbit, const Rabbit&, const Position& rp) {
                const glm::vec2 d = rp.now - p.now;
                const float dist2 = d.x * d.x + d.y * d.y;
                if (dist2 < best && !world.has<Dying>(rabbit)) {
                    best = dist2;
                    target = rabbit;
                    target_at = rp.now;
                }
            });

            if (target) {
                const glm::vec2 to = target_at - p.now;
                h.radians = std::atan2(to.y, to.x);
                if (best <= fox_reach * fox_reach && fox.cooldown == 0) {
                    hunts.emit(HuntEvent{.predator_index = me.index, .predator_generation = me.generation,
                                         .prey_index = target.index, .prey_generation = target.generation});
                    fox.cooldown = fox_hunt_cooldown;
                }
                step(p, h, std::min(fox_rules.speed, std::sqrt(best)));
            } else {
                h.radians += turn(rng);
                step(p, h, fox_rules.speed * 0.7f);
            }

            energy.value -= fox_rules.metabolism;
            if (energy.value >= fox_rules.breed_at) {
                energy.value -= fox_rules.breed_cost;
                births.emit(BirthRequestEvent{.species = static_cast<std::uint32_t>(Species::Fox), .x = p.now.x,
                                              .y = p.now.y, .energy = fox_rules.newborn_energy});
            }
            if (energy.value <= 0.0f) {
                world.emplace<Dying>(me);
                starved.emit(StarvedEvent{.index = me.index, .generation = me.generation});
            }
        });
    }
};

/// Life — единственный, кто создаёт и уничтожает сущности. Проверяет каждую ссылку через world.valid().
struct Life {
    es::EventReader<HuntEvent> hunts;
    es::EventReader<StarvedEvent> starved;
    es::EventReader<BirthRequestEvent> requests;
    es::EventWriter<BornEvent> born;
    es::EventWriter<DiedEvent> died;
    std::mt19937 rng{404};

    std::uint64_t stale_hunts = 0;     ///< Охоты на уже мёртвого зайца или мёртвой лисой.
    std::uint64_t rejected_births = 0; ///< Отклонены из-за предела популяции.

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Life")
                                    .consumes<HuntEvent>()
                                    .consumes<StarvedEvent>()
                                    .consumes<BirthRequestEvent>()
                                    .produces<BornEvent>(es::ChannelConfig{.reserve = 512, .max_events_per_tick = 4096})
                                    .produces<DiedEvent>(es::ChannelConfig{.reserve = 512, .max_events_per_tick = 4096});
        hunts = bus.reader<HuntEvent>(id);
        starved = bus.reader<StarvedEvent>(id);
        requests = bus.reader<BirthRequestEvent>(id);
        born = bus.writer<BornEvent>(id);
        died = bus.writer<DiedEvent>(id);
    }

    void tick(ECS::World& world) {
        for (const HuntEvent& hunt : hunts.events()) {
            // Заяц мог умереть от голода, достаться другой лисе или его слот уже занят новым зайцем.
            if (!world.valid(hunt.prey()) || !world.valid(hunt.predator())) {
                ++stale_hunts;
                continue;
            }
            kill(world, hunt.prey(), Species::Rabbit, Cause::Eaten, hunt.predator());
        }
        for (const StarvedEvent& s : starved.events()) {
            const ECS::Entity e = entity(s.index, s.generation);
            if (!world.valid(e)) continue;
            kill(world, e, world.has<Fox>(e) ? Species::Fox : Species::Rabbit, Cause::Starved, ECS::Entity{});
        }
        std::uniform_real_distribution<float> angle(0.0f, 2.0f * std::numbers::pi_v<float>);
        for (const BirthRequestEvent& request : requests.events()) {
            const auto species = static_cast<Species>(request.species);
            const bool is_fox = species == Species::Fox;
            const std::size_t population = is_fox ? world.count<Fox>() : world.count<Rabbit>();
            if (population >= (is_fox ? fox_rules.cap : rabbit_rules.cap)) {
                ++rejected_births;
                continue;
            }
            const ECS::Entity e = world.create();
            const glm::vec2 at{request.x, request.y};
            world.emplace<Position>(e, at, at);
            world.emplace<Heading>(e, angle(rng));
            world.emplace<Energy>(e, request.energy);
            if (is_fox) world.emplace<Fox>(e);
            else world.emplace<Rabbit>(e);
            born.emit(BornEvent{.index = e.index, .generation = e.generation, .species = request.species});
        }
    }

private:
    void kill(ECS::World& world, ECS::Entity e, Species species, Cause cause, ECS::Entity killer) {
        world.destroy(e); // поколение слота растёт: все старые ссылки на него невалидны
        died.emit(DiedEvent{.index = e.index, .generation = e.generation, .species = static_cast<std::uint32_t>(species),
                            .cause = static_cast<std::uint32_t>(cause), .killer_index = killer.index,
                            .killer_generation = killer.generation});
    }
};

/// Census — считает популяции только по событиям и ведёт историю для графика.
struct Census {
    es::EventReader<BornEvent> born;
    es::EventReader<DiedEvent> died;

    std::array<int, 2> alive{};
    std::array<std::uint64_t, 2> births_total{};
    std::array<std::uint64_t, 2> starved_total{};
    std::uint64_t eaten_total = 0;
    std::array<int, 2> min_alive{1 << 30, 1 << 30};
    std::array<int, 2> max_alive{};

    static constexpr std::size_t history_size = 480;
    static constexpr es::Tick sample_every = 4;
    std::vector<std::array<int, 2>> history; ///< Кольцевой буфер: одна точка на sample_every тиков.
    std::size_t history_head = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Census").consumes<BornEvent>().consumes<DiedEvent>();
        born = bus.reader<BornEvent>(id);
        died = bus.reader<DiedEvent>(id);
        history.reserve(history_size);
    }

    void tick(es::Tick now, const Life& life) {
        for (const BornEvent& b : born.events()) {
            ++alive[b.species];
            ++births_total[b.species];
        }
        for (const DiedEvent& d : died.events()) {
            --alive[d.species];
            if (static_cast<Cause>(d.cause) == Cause::Eaten) ++eaten_total;
            else ++starved_total[d.species];
        }
        if (now > 60) { // стартовая популяция ещё рождается
            for (std::size_t s = 0; s < 2; ++s) {
                min_alive[s] = std::min(min_alive[s], alive[s]);
                max_alive[s] = std::max(max_alive[s], alive[s]);
            }
        }

        if (now % sample_every == 0) {
            if (history.size() < history_size) {
                history.push_back(alive);
            } else {
                history[history_head] = alive;
                history_head = (history_head + 1) % history_size;
            }
        }
        if (now > 0 && now % 300 == 0) {
            std::println("[tick {:>5}] Census: rabbits {:>4}, foxes {:>3} | eaten {}, starved {}/{} | stale hunts {}",
                         now, alive[0], alive[1], eaten_total, starved_total[0], starved_total[1], life.stale_hunts);
        }
    }

    /// График численности: зайцы — светлая линия, лисы — оранжевая. Шкала общая, по максимуму в истории.
    void render_overlay(Renderer2D& r, glm::vec2 viewport) const {
        const Rect panel{{viewport.x - 332.0f, viewport.y - 132.0f}, {320.0f, 120.0f}};
        r.fill_rect(panel, Color{0, 0, 0, 150}, 0);
        if (history.size() < 2) return;

        int peak = 10;
        for (const auto& point : history) peak = std::max({peak, point[0], point[1]});
        const float dx = panel.size.x / static_cast<float>(history_size - 1);
        auto at = [&](std::size_t i, std::size_t species) {
            const std::size_t k = history.size() < history_size ? i : (history_head + i) % history_size;
            const float value = static_cast<float>(history[k][species]) / static_cast<float>(peak);
            return glm::vec2{panel.position.x + dx * static_cast<float>(i), panel.max().y - 4.0f - value * (panel.size.y - 8.0f)};
        };
        constexpr std::array<std::uint32_t, 2> colors = {0xE8E4D8FF, 0xF08A3CFF};
        for (std::size_t species = 0; species < 2; ++species) {
            for (std::size_t i = 1; i < history.size(); ++i) {
                r.draw_line(at(i - 1, species), at(i, species), 1.5f, Color::from_rgba(colors[species]), 1);
            }
        }
    }
};

/// Migration — если вид почти исчез, приводит несколько особей с края карты.
struct Migration {
    es::EventWriter<BirthRequestEvent> arrivals;
    es::Tick next_allowed = 0;
    std::uint64_t arrivals_total = 0;
    std::mt19937 rng{505};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Migration").produces<BirthRequestEvent>();
        arrivals = bus.writer<BirthRequestEvent>(id);
    }

    void tick(es::Tick now, const Census& census) {
        // Census считает с задержкой в пару тиков: не вмешиваемся, пока рождается стартовая популяция,
        // и после миграции ждём, пока цифры догонят.
        if (now < 60 || now < next_allowed) return;
        std::uniform_real_distribution<float> edge_y(0.0f, static_cast<float>(grid_h) - 0.01f);
        auto arrive = [&](Species species, int count, float energy) {
            for (int i = 0; i < count; ++i) {
                arrivals.emit(BirthRequestEvent{.species = static_cast<std::uint32_t>(species), .x = 0.5f,
                                                .y = edge_y(rng), .energy = energy});
            }
            arrivals_total += static_cast<std::uint64_t>(count);
            next_allowed = now + 90;
        };
        if (census.alive[0] < 10) arrive(Species::Rabbit, 12, rabbit_rules.newborn_energy * 2.0f);
        if (census.alive[1] < 2) arrive(Species::Fox, 3, fox_rules.newborn_energy * 2.0f);
    }
};

/// Spawner — стартовая популяция и существа, которых выпускает игрок.
struct Spawner {
    es::EventReader<Core::MouseButtonEvent> mouse;
    es::EventWriter<BirthRequestEvent> requests;
    std::mt19937 rng{606};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Spawner")
                                    .consumes<Core::MouseButtonEvent>()
                                    .produces<BirthRequestEvent>();
        mouse = bus.reader<Core::MouseButtonEvent>(id);
        requests = bus.writer<BirthRequestEvent>(id);
    }

    void seed() {
        std::uniform_real_distribution<float> x(0.0f, static_cast<float>(grid_w) - 0.01f);
        std::uniform_real_distribution<float> y(0.0f, static_cast<float>(grid_h) - 0.01f);
        for (int i = 0; i < 160; ++i) request(Species::Rabbit, {x(rng), y(rng)}, 1.0f);
        for (int i = 0; i < 14; ++i) request(Species::Fox, {x(rng), y(rng)}, 1.5f);
    }

    void tick() {
        std::uniform_real_distribution<float> spread(-2.0f, 2.0f);
        for (const Core::MouseButtonEvent& click : mouse.events()) {
            if (!click.pressed() || click.which() == MouseButton::Middle) continue;
            const glm::vec2 at = glm::vec2{click.world_x, click.world_y} / cell_size;
            if (at.x < 0.0f || at.y < 0.0f || at.x >= grid_w || at.y >= grid_h) continue;
            const bool rabbits = click.which() == MouseButton::Left;
            for (int i = 0; i < (rabbits ? 10 : 3); ++i) {
                const glm::vec2 p = glm::clamp(at + glm::vec2{spread(rng), spread(rng)}, glm::vec2(0.0f),
                                               glm::vec2(grid_w, grid_h) - 0.01f);
                request(rabbits ? Species::Rabbit : Species::Fox, p, rabbits ? 1.0f : 1.5f);
            }
        }
    }

private:
    void request(Species species, glm::vec2 p, float energy) {
        requests.emit(BirthRequestEvent{.species = static_cast<std::uint32_t>(species), .x = p.x, .y = p.y, .energy = energy});
    }
};

// =============================================================================
// Игра: хранит мир и модули, вызывает их по порядку, рисует. Своей логики у неё нет.
// =============================================================================

class Ecosystem final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {grid_w * cell_size, grid_h * cell_size}; }

    void setup(Core::App& app) override {
        es::EventBus& bus = app.bus();
        spawner.declare(bus);
        grass.declare(bus);
        rabbits.declare(bus);
        foxes.declare(bus);
        life.declare(bus);
        census.declare(bus);
        migration.declare(bus);

        grass.setup();
        spawner.seed();
    }

    void tick(Core::App& app) override {
        spawner.tick();
        grass.tick();
        rabbits.tick(world, grass);
        foxes.tick(world);
        life.tick(world);
        census.tick(app.tick(), life);
        migration.tick(app.tick(), census);
    }

    void render(Core::App& app, Renderer2D& r) override {
        r.fill_rect({{0.0f, 0.0f}, world_size()}, Color::from_rgba(0x221D16FF), -10);
        grass.render(r);
        const float alpha = app.tick_alpha();
        auto draw = [&](ECS::Entity e, const Position& p, float size, Color color, std::int32_t layer) {
            const glm::vec2 at = glm::mix(p.prev, p.now, alpha) * cell_size;
            r.fill_rect({at - size * 0.5f, {size, size}}, world.has<Dying>(e) ? color.with_alpha(110) : color, layer);
        };
        world.view<const Rabbit, const Position>().each([&](ECS::Entity e, const Rabbit&, const Position& p) {
            draw(e, p, 4.0f, Color::from_rgba(0xE8E4D8FF), 2);
        });
        world.view<const Fox, const Position>().each([&](ECS::Entity e, const Fox&, const Position& p) {
            draw(e, p, 7.0f, Color::from_rgba(0xF08A3CFF), 3);
        });
    }

    void render_overlay(Core::App& app, Renderer2D& r) override { census.render_overlay(r, app.camera().viewport); }

    [[nodiscard]] std::string status() const override {
        return std::format("rabbits {} | foxes {} | eaten {} | stale hunts {}", census.alive[0], census.alive[1],
                           census.eaten_total, life.stale_hunts);
    }

    void shutdown(Core::App& app) override {
        std::println("\n===== Ecosystem : summary after {} ticks =====", app.tick());
        std::println("rabbits: now {}, min {}, max {}, born {}", census.alive[0], census.min_alive[0], census.max_alive[0],
                     census.births_total[0]);
        std::println("foxes:   now {}, min {}, max {}, born {}", census.alive[1], census.min_alive[1], census.max_alive[1],
                     census.births_total[1]);
        std::println("eaten {}, stale hunts {}, migrants {}, rejected births {}", census.eaten_total, life.stale_hunts,
                     migration.arrivals_total, life.rejected_births);
        std::println("ECS: {} entities alive, {} slots ever used", world.alive(), world.registry().slots());
    }

private:
    ECS::World world;
    Spawner spawner;
    Grass grass;
    Rabbits rabbits;
    Foxes foxes;
    Life life;
    Census census;
    Migration migration;
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<Ecosystem>({.title = "Ecosystem", .ticks_per_second = 30.0}, argc, argv);
}
