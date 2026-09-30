/**
 * @file main.cpp
 * @brief Ecosystem — трава, зайцы и лисы: популяции, которые рождаются и умирают.
 *
 * Что показывает:
 * - жизненный цикл сущностей: сотни рождений и смертей, слоты переиспользуются;
 * - ссылки с поколением (Handle): лиса отправляет `eco.hunt` в тике N, Life разбирает его
 *   в тике N+1 — за это время заяц мог умереть, а его слот достаться новорождённому.
 *   Проверка поколения отбрасывает такие «устаревшие охоты» (счётчик stale в заголовке);
 * - модули-структуры: у каждого модуля свои порты, данные и declare(), чужие данные
 *   приходят в tick() как const& (Rabbits читает Grass, Foxes читает Rabbits);
 * - интерполяция отрисовки между тиками (App::tick_alpha).
 *
 *   Spawner ──┐                        ┌──▶ eco.born ──▶ Rabbits, Foxes, Census
 *   Rabbits ──┼─ eco.birth_request ─▶ Life
 *   Foxes ────┘   eco.hunt, eco.starved ─▶ Life ──▶ eco.died ──▶ Rabbits, Foxes, Census
 *   Migration ─ eco.birth_request ─▶ Life                   Rabbits ── eco.grazed ──▶ Grass
 *
 * Управление: ЛКМ — выпустить зайцев, ПКМ — выпустить лис. Общие клавиши — см. Core::App.
 */

#include <Core/Core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <numbers>
#include <print>
#include <random>
#include <string_view>
#include <vector>

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

/// Ссылка на существо: слот в пуле Life + поколение слота.
/// Слот переиспользуется после смерти, поколение при этом растёт — старая ссылка перестаёт быть валидной.
struct Handle {
    static constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t index = none;
    std::uint32_t generation = 0;
    friend bool operator==(Handle, Handle) = default;
};

std::size_t cell_index(int x, int y) { return static_cast<std::size_t>(y) * grid_w + static_cast<std::size_t>(x); }
glm::ivec2 cell_of(glm::vec2 p) {
    return {std::clamp(static_cast<int>(p.x), 0, grid_w - 1), std::clamp(static_cast<int>(p.y), 0, grid_h - 1)};
}

// =============================================================================
// События. Handle передаётся двумя полями: события — плоские структуры.
// =============================================================================

/// Заяц съел траву в клетке.
struct GrazedEvent {
    std::int32_t x = 0;
    std::int32_t y = 0;

    static constexpr std::string_view event_name = "eco.grazed";
    using fields = es::Fields<es::Field<"x", &GrazedEvent::x>, es::Field<"y", &GrazedEvent::y>>;
};

/// Просьба о рождении: слот выдаёт только Life.
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

    [[nodiscard]] Handle predator() const { return {predator_index, predator_generation}; }
    [[nodiscard]] Handle prey() const { return {prey_index, prey_generation}; }

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

/// Существо родилось и получило слот.
struct BornEvent {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    std::uint32_t species = 0;
    float x = 0.0f;
    float y = 0.0f;
    float energy = 0.0f;

    [[nodiscard]] Handle handle() const { return {index, generation}; }

    static constexpr std::string_view event_name = "eco.born";
    using fields = es::Fields<es::Field<"index", &BornEvent::index>, es::Field<"generation", &BornEvent::generation>,
                              es::Field<"species", &BornEvent::species>, es::Field<"x", &BornEvent::x>,
                              es::Field<"y", &BornEvent::y>, es::Field<"energy", &BornEvent::energy>>;
};

/// Существо умерло; слот уже освобождён. Для съеденных — кто съел.
struct DiedEvent {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    std::uint32_t species = 0;
    std::uint32_t cause = 0;
    std::uint32_t killer_index = Handle::none;
    std::uint32_t killer_generation = 0;

    [[nodiscard]] Handle handle() const { return {index, generation}; }
    [[nodiscard]] Handle killer() const { return {killer_index, killer_generation}; }

    static constexpr std::string_view event_name = "eco.died";
    using fields = es::Fields<es::Field<"index", &DiedEvent::index>, es::Field<"generation", &DiedEvent::generation>,
                              es::Field<"species", &DiedEvent::species>, es::Field<"cause", &DiedEvent::cause>,
                              es::Field<"killer_index", &DiedEvent::killer_index>,
                              es::Field<"killer_generation", &DiedEvent::killer_generation>>;
};

// =============================================================================
// Herd — SoA-массивы существ одного вида. Это то, что потом станет компонентами ECS.
// =============================================================================

struct Herd {
    std::vector<Handle> handle;
    std::vector<glm::vec2> pos;
    std::vector<glm::vec2> prev; ///< Позиция в прошлом тике — для интерполяции отрисовки.
    std::vector<float> heading;  ///< Направление, радианы.
    std::vector<float> energy;
    std::vector<int> cooldown;   ///< Лисы: тиков до следующей атаки.
    std::vector<std::uint8_t> dying; ///< Отправлен eco.starved, ждём eco.died.
    std::vector<std::uint32_t> row_of; ///< Слот Life → строка массивов (или Handle::none).

    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    [[nodiscard]] std::size_t size() const { return handle.size(); }

    [[nodiscard]] std::size_t find(Handle h) const {
        if (h.index >= row_of.size() || row_of[h.index] == Handle::none) return npos;
        const std::size_t row = row_of[h.index];
        return handle[row] == h ? row : npos;
    }

    void add(Handle h, glm::vec2 p, float e, float head) {
        if (row_of.size() <= h.index) row_of.resize(h.index + 1, Handle::none);
        row_of[h.index] = static_cast<std::uint32_t>(handle.size());
        handle.push_back(h);
        pos.push_back(p);
        prev.push_back(p);
        heading.push_back(head);
        energy.push_back(e);
        cooldown.push_back(0);
        dying.push_back(0);
    }

    /// Удаление перестановкой последней строки на место удалённой: массивы остаются плотными.
    void remove(Handle h) {
        const std::size_t row = find(h);
        if (row == npos) return;
        const std::size_t last = size() - 1;
        row_of[h.index] = Handle::none;
        if (row != last) {
            handle[row] = handle[last];
            pos[row] = pos[last];
            prev[row] = prev[last];
            heading[row] = heading[last];
            energy[row] = energy[last];
            cooldown[row] = cooldown[last];
            dying[row] = dying[last];
            row_of[handle[row].index] = static_cast<std::uint32_t>(row);
        }
        handle.pop_back();
        pos.pop_back();
        prev.pop_back();
        heading.pop_back();
        energy.pop_back();
        cooldown.pop_back();
        dying.pop_back();
    }

    /// Шаг по направлению; от краёв мира отражаемся.
    void step(std::size_t i, float speed) {
        glm::vec2 next = pos[i] + glm::vec2{std::cos(heading[i]), std::sin(heading[i])} * speed;
        if (next.x < 0.0f || next.x >= static_cast<float>(grid_w)) {
            heading[i] = std::numbers::pi_v<float> - heading[i];
            next.x = std::clamp(next.x, 0.0f, static_cast<float>(grid_w) - 0.01f);
        }
        if (next.y < 0.0f || next.y >= static_cast<float>(grid_h)) {
            heading[i] = -heading[i];
            next.y = std::clamp(next.y, 0.0f, static_cast<float>(grid_h) - 0.01f);
        }
        pos[i] = next;
    }

    void render(Renderer2D& r, float alpha, float size, Color color, std::int32_t layer) const {
        for (std::size_t i = 0; i < this->size(); ++i) {
            const glm::vec2 p = glm::mix(prev[i], pos[i], alpha) * cell_size;
            r.fill_rect({p - size * 0.5f, {size, size}}, dying[i] != 0 ? color.with_alpha(110) : color, layer);
        }
    }
};

// =============================================================================
// Модули. У каждого: порты (reader/writer), свои данные, declare() и tick().
// =============================================================================

/// Трава: съедается по событиям, отрастает сама.
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

/// Зайцы: бродят, тянутся к траве, едят, размножаются.
struct Rabbits {
    es::EventReader<BornEvent> born;
    es::EventReader<DiedEvent> died;
    es::EventWriter<GrazedEvent> grazed;
    es::EventWriter<BirthRequestEvent> births;
    es::EventWriter<StarvedEvent> starved;
    Herd herd;
    std::mt19937 rng{202};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Rabbits")
                                    .consumes<BornEvent>()
                                    .consumes<DiedEvent>()
                                    .produces<GrazedEvent>(es::ChannelConfig{.reserve = 2048, .max_events_per_tick = 4096})
                                    .produces<BirthRequestEvent>(es::ChannelConfig{.reserve = 512, .max_events_per_tick = 4096})
                                    .produces<StarvedEvent>(es::ChannelConfig{.reserve = 256, .max_events_per_tick = 4096});
        born = bus.reader<BornEvent>(id);
        died = bus.reader<DiedEvent>(id);
        grazed = bus.writer<GrazedEvent>(id);
        births = bus.writer<BirthRequestEvent>(id);
        starved = bus.writer<StarvedEvent>(id);
    }

    void tick(const Grass& grass) {
        // Сначала смерти, потом рождения: слот, освобождённый в прошлом тике, мог уже достаться новорождённому.
        for (const DiedEvent& d : died.events()) {
            if (static_cast<Species>(d.species) == Species::Rabbit) herd.remove(d.handle());
        }
        std::uniform_real_distribution<float> angle(0.0f, 2.0f * std::numbers::pi_v<float>);
        for (const BornEvent& b : born.events()) {
            if (static_cast<Species>(b.species) == Species::Rabbit) herd.add(b.handle(), {b.x, b.y}, b.energy, angle(rng));
        }

        std::uniform_real_distribution<float> turn(-0.6f, 0.6f);
        std::uniform_real_distribution<float> jitter(-0.5f, 0.5f);
        for (std::size_t i = 0; i < herd.size(); ++i) {
            herd.prev[i] = herd.pos[i];
            if (herd.dying[i] != 0) continue;

            // Тянемся к самой густой траве среди соседних клеток, иначе бредём.
            const glm::ivec2 here = cell_of(herd.pos[i]);
            glm::ivec2 best = here;
            for (const glm::ivec2 d : {glm::ivec2{1, 0}, glm::ivec2{-1, 0}, glm::ivec2{0, 1}, glm::ivec2{0, -1}}) {
                const glm::ivec2 n = here + d;
                if (n.x >= 0 && n.y >= 0 && n.x < grid_w && n.y < grid_h && grass.at(n) > grass.at(best)) best = n;
            }
            if (best != here) {
                const glm::vec2 to = glm::vec2(best) + 0.5f + glm::vec2{jitter(rng), jitter(rng)} - herd.pos[i];
                herd.heading[i] = std::atan2(to.y, to.x);
            } else {
                herd.heading[i] += turn(rng);
            }
            herd.step(i, rabbit_rules.speed);

            float& e = herd.energy[i];
            e -= rabbit_rules.metabolism;
            const glm::ivec2 c = cell_of(herd.pos[i]);
            if (grass.at(c) > 0 && e < rabbit_rules.max_energy) {
                grazed.emit(GrazedEvent{.x = c.x, .y = c.y});
                e += rabbit_rules.food_gain;
            }
            if (e >= rabbit_rules.breed_at) {
                e -= rabbit_rules.breed_cost;
                births.emit(BirthRequestEvent{.species = static_cast<std::uint32_t>(Species::Rabbit), .x = herd.pos[i].x,
                                              .y = herd.pos[i].y, .energy = rabbit_rules.newborn_energy});
            }
            if (e <= 0.0f) {
                herd.dying[i] = 1;
                starved.emit(StarvedEvent{.index = herd.handle[i].index, .generation = herd.handle[i].generation});
            }
        }
    }
};

/// Лисы: ищут ближайшего зайца, догоняют, охотятся. Сытость — только за засчитанную охоту.
struct Foxes {
    es::EventReader<BornEvent> born;
    es::EventReader<DiedEvent> died;
    es::EventWriter<HuntEvent> hunts;
    es::EventWriter<BirthRequestEvent> births;
    es::EventWriter<StarvedEvent> starved;
    Herd herd;
    std::mt19937 rng{303};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Foxes")
                                    .consumes<BornEvent>()
                                    .consumes<DiedEvent>()
                                    .produces<HuntEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 1024})
                                    .produces<BirthRequestEvent>()
                                    .produces<StarvedEvent>();
        born = bus.reader<BornEvent>(id);
        died = bus.reader<DiedEvent>(id);
        hunts = bus.writer<HuntEvent>(id);
        births = bus.writer<BirthRequestEvent>(id);
        starved = bus.writer<StarvedEvent>(id);
    }

    void tick(const Rabbits& rabbits) {
        for (const DiedEvent& d : died.events()) {
            const auto species = static_cast<Species>(d.species);
            if (species == Species::Fox) {
                herd.remove(d.handle());
            } else if (static_cast<Cause>(d.cause) == Cause::Eaten) {
                // Охота засчитана: кормим охотника, если он сам ещё жив.
                if (const std::size_t row = herd.find(d.killer()); row != Herd::npos) {
                    herd.energy[row] = std::min(herd.energy[row] + fox_rules.food_gain, fox_rules.max_energy);
                }
            }
        }
        std::uniform_real_distribution<float> angle(0.0f, 2.0f * std::numbers::pi_v<float>);
        for (const BornEvent& b : born.events()) {
            if (static_cast<Species>(b.species) == Species::Fox) herd.add(b.handle(), {b.x, b.y}, b.energy, angle(rng));
        }

        const Herd& prey = rabbits.herd;
        std::uniform_real_distribution<float> turn(-0.4f, 0.4f);
        for (std::size_t i = 0; i < herd.size(); ++i) {
            herd.prev[i] = herd.pos[i];
            if (herd.dying[i] != 0) continue;
            if (herd.cooldown[i] > 0) --herd.cooldown[i];

            // Ближайший живой заяц в поле зрения.
            std::size_t target = Herd::npos;
            float best = fox_sight * fox_sight;
            for (std::size_t k = 0; k < prey.size(); ++k) {
                if (prey.dying[k] != 0) continue;
                const glm::vec2 d = prey.pos[k] - herd.pos[i];
                const float dist2 = d.x * d.x + d.y * d.y;
                if (dist2 < best) {
                    best = dist2;
                    target = k;
                }
            }

            if (target != Herd::npos) {
                const glm::vec2 to = prey.pos[target] - herd.pos[i];
                herd.heading[i] = std::atan2(to.y, to.x);
                if (best <= fox_reach * fox_reach && herd.cooldown[i] == 0) {
                    const Handle me = herd.handle[i];
                    const Handle victim = prey.handle[target];
                    hunts.emit(HuntEvent{.predator_index = me.index, .predator_generation = me.generation,
                                         .prey_index = victim.index, .prey_generation = victim.generation});
                    herd.cooldown[i] = fox_hunt_cooldown;
                }
                herd.step(i, std::min(fox_rules.speed, std::sqrt(best)));
            } else {
                herd.heading[i] += turn(rng);
                herd.step(i, fox_rules.speed * 0.7f);
            }

            float& e = herd.energy[i];
            e -= fox_rules.metabolism;
            if (e >= fox_rules.breed_at) {
                e -= fox_rules.breed_cost;
                births.emit(BirthRequestEvent{.species = static_cast<std::uint32_t>(Species::Fox), .x = herd.pos[i].x,
                                              .y = herd.pos[i].y, .energy = fox_rules.newborn_energy});
            }
            if (e <= 0.0f) {
                herd.dying[i] = 1;
                starved.emit(StarvedEvent{.index = herd.handle[i].index, .generation = herd.handle[i].generation});
            }
        }
    }
};

/// Life — единственный, кто выдаёт и освобождает слоты. Проверяет поколение каждой ссылки.
struct Life {
    es::EventReader<HuntEvent> hunts;
    es::EventReader<StarvedEvent> starved;
    es::EventReader<BirthRequestEvent> requests;
    es::EventWriter<BornEvent> born;
    es::EventWriter<DiedEvent> died;

    std::vector<std::uint32_t> generation;
    std::vector<std::uint8_t> alive;
    std::vector<std::uint32_t> free_slots;
    std::array<std::size_t, 2> population{};
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

    [[nodiscard]] bool valid(Handle h) const {
        return h.index < generation.size() && alive[h.index] != 0 && generation[h.index] == h.generation;
    }

    void tick() {
        for (const HuntEvent& hunt : hunts.events()) {
            // Заяц мог умереть от голода, достаться другой лисе или его слот уже занят новым зайцем.
            if (!valid(hunt.prey()) || !valid(hunt.predator())) {
                ++stale_hunts;
                continue;
            }
            release(hunt.prey(), Species::Rabbit, Cause::Eaten, hunt.predator());
        }
        for (const StarvedEvent& s : starved.events()) {
            const Handle h{s.index, s.generation};
            if (valid(h)) release(h, species_of(h), Cause::Starved, Handle{});
        }
        for (const BirthRequestEvent& request : requests.events()) {
            const auto species = static_cast<Species>(request.species);
            const SpeciesRules& rules = species == Species::Rabbit ? rabbit_rules : fox_rules;
            if (population[request.species] >= rules.cap) {
                ++rejected_births;
                continue;
            }
            const Handle h = allocate(species);
            born.emit(BornEvent{.index = h.index, .generation = h.generation, .species = request.species, .x = request.x,
                                .y = request.y, .energy = request.energy});
        }
    }

private:
    std::vector<Species> m_species;

    [[nodiscard]] Species species_of(Handle h) const { return m_species[h.index]; }

    Handle allocate(Species species) {
        std::uint32_t index = 0;
        if (!free_slots.empty()) {
            index = free_slots.back();
            free_slots.pop_back();
        } else {
            index = static_cast<std::uint32_t>(generation.size());
            generation.push_back(0);
            alive.push_back(0);
            m_species.push_back(species);
        }
        alive[index] = 1;
        m_species[index] = species;
        ++population[static_cast<std::size_t>(species)];
        return {index, generation[index]};
    }

    void release(Handle h, Species species, Cause cause, Handle killer) {
        alive[h.index] = 0;
        ++generation[h.index]; // все старые ссылки на этот слот становятся невалидными
        free_slots.push_back(h.index);
        --population[static_cast<std::size_t>(species)];
        died.emit(DiedEvent{.index = h.index, .generation = h.generation, .species = static_cast<std::uint32_t>(species),
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
    std::mt19937 rng{404};

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Migration").produces<BirthRequestEvent>();
        arrivals = bus.writer<BirthRequestEvent>(id);
    }

    void tick(es::Tick now, const Census& census) {
        // Census считает с задержкой в пару тиков, поэтому после миграции ждём, пока цифры догонят.
        if (now < next_allowed) return;
        std::uniform_real_distribution<float> edge_y(0.0f, static_cast<float>(grid_h) - 0.01f);
        auto arrive = [&](Species species, int count, float energy) {
            for (int i = 0; i < count; ++i) {
                arrivals.emit(BirthRequestEvent{.species = static_cast<std::uint32_t>(species), .x = 0.5f,
                                                .y = edge_y(rng), .energy = energy});
            }
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
    std::mt19937 rng{505};

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
            if (click.action != GLFW_PRESS || click.button == GLFW_MOUSE_BUTTON_MIDDLE) continue;
            const glm::vec2 at = glm::vec2{click.world_x, click.world_y} / cell_size;
            if (at.x < 0.0f || at.y < 0.0f || at.x >= grid_w || at.y >= grid_h) continue;
            const bool rabbits = click.button == GLFW_MOUSE_BUTTON_LEFT;
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
// Игра: хранит модули, вызывает их по порядку, рисует. Своей логики у неё нет.
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
        rabbits.tick(grass);
        foxes.tick(rabbits);
        life.tick();
        census.tick(app.tick(), life);
        migration.tick(app.tick(), census);
    }

    void render(Core::App& app, Renderer2D& r) override {
        r.fill_rect({{0.0f, 0.0f}, world_size()}, Color::from_rgba(0x221D16FF), -10);
        grass.render(r);
        const float alpha = app.tick_alpha();
        rabbits.herd.render(r, alpha, 4.0f, Color::from_rgba(0xE8E4D8FF), 2);
        foxes.herd.render(r, alpha, 7.0f, Color::from_rgba(0xF08A3CFF), 3);
    }

    void render_overlay(Core::App& app, Renderer2D& r) override { census.render_overlay(r, app.camera().viewport); }

    [[nodiscard]] std::string status() const override {
        return std::format("rabbits {} | foxes {} | eaten {} | stale hunts {}", census.alive[0], census.alive[1],
                           census.eaten_total, life.stale_hunts);
    }

private:
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
