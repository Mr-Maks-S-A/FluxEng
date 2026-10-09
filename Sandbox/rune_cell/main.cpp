/**
 * @file main.cpp
 * @brief RuneCell — заклинание как живая клетка: срез под микроскопом, руны — атомы, связи — как в химии.
 *
 * Идея (биология + химия → магия):
 * - **Мембрана** — форма заклинания: мягкое кольцо из узлов (пружины, изгиб, давление цитоплазмы).
 *   Чем больше рун, тем больше клетка; Земля делает оболочку жёстче, Воздух — разгоняет брошенное заклинание.
 * - **Ядро** — стихия клетки: выпускает в цитоплазму частицы маны.
 * - **Руны — атомы с валентностью**: Огонь 2, Вода 2, Земля 4, Воздух 1, Свет 3. Связи бывают одинарные,
 *   двойные и тройные и расходуют валентность. Связанные руны — **молекула**. Молекула, у которой
 *   вся валентность занята, **устойчива**: её руны поглощают ману. Руны со свободной валентностью — **радикалы**:
 *   мерцают, теряют энергию и сами вступают в реакции с соседними радикалами.
 * - **Кольца** (цикл в молекуле, как бензол: шесть Земель через одну двойную связь + шесть Воздухов)
 *   — «ароматика»: дают заклинанию резонанс (+50% силы).
 * - **Обмен веществ**: ядро → частицы маны → устойчивые руны → энергия течёт по связям (диффузия по графу).
 * - **Деление**: накопив энергию, клетка делится надвое (митоз): руны расходятся по сторонам, разрезанные связи рвутся.
 * - **Заклинание**: Space — клетка срывается с места и летит в голема. Урон — от Огня, лечение — от Воды,
 *   скорость — от Воздуха, усиление — от Света и колец. Формула показывается как химическая: «Ig₂Ve₂ · Te₆Ve₆◯».
 *
 * Модули:
 * - **ECSSystem** — руны, связи (ссылки на руны с поколением!) и клетки — сущности; один владелец структуры — Chemistry;
 * - **EventSystem** — ввод → `rune.inject`, `rune.bond_request` → Chemistry → `rune.bond_changed`, `cell.divided`;
 *   Metabolism → `cell.division_request`; Caster → `spell.cast`, `spell.hit` → Golem, Chronicle;
 * - **JobSystem** — физика клеток (кусок = клетка) и движение тысяч частиц маны (поглощения — через ChunkBuffers):
 *   результат одинаков при любом `--threads`;
 * - **MemorySystem** — частицы маны в `Pool`, списки рун по клеткам — в памяти тика;
 * - **RendererSystem** — только Renderer2D (на OpenGL и Vulkan: `--backend gl|vulkan`), текст, фон-агар из шума;
 * - **Core / WindowSystem** — окно, тик 60 Гц, мышь и клавиши → события шины.
 *
 * Управление: 1–5 — руна, ЛКМ в клетке — посадить руну, ПКМ с руны на руну — связь (повторно — кратнее),
 * ПКМ по руне без перетаскивания — разорвать её самую слабую связь, X — удалить руну под курсором,
 * Space — запустить клетку под курсором, N — новая клетка, A — автоигра, P — пауза.
 * Аргументы: `--autoplay`, `--seed N`. Общие (`--ticks`, `--threads`, `--backend`, `--screenshot`) — см. Core::App.
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
#include <limits>
#include <numbers>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using InputSystem::Key;
using InputSystem::MouseButton;

namespace es = EventSystem;
namespace ms = MemorySystem;
namespace js = JobSystem;
using namespace RendererSystem;

namespace {

constexpr float pi = std::numbers::pi_v<float>;
constexpr glm::vec2 world_size{1600.0f, 900.0f};
constexpr glm::vec2 dish_center{520.0f, 450.0f};
constexpr float dish_radius = 410.0f;
constexpr glm::vec2 golem_center{1260.0f, 300.0f};
constexpr float golem_radius = 70.0f;
constexpr int membrane_nodes = 40;
constexpr float rune_radius = 13.0f;
constexpr float nucleus_radius = 20.0f;
constexpr std::size_t max_cells = 12;
constexpr std::size_t max_runes_per_cell = 40;

// =============================================================================
// Руны
// =============================================================================

enum class Element : std::uint8_t { Ignis, Aqua, Terra, Ventus, Lux, Count };

struct ElementInfo {
    std::string_view name;
    std::string_view symbol;
    std::string_view effect;
    std::uint8_t valence;
    std::uint32_t color;
};

constexpr std::array<ElementInfo, 5> elements{{
    {"Огонь", "Ig", "урон", 2, 0xFF6A2AFF},
    {"Вода", "Aq", "лечение", 2, 0x3FA9F5FF},
    {"Земля", "Te", "прочность", 4, 0xB08850FF},
    {"Воздух", "Ve", "скорость", 1, 0xC8F6FFFF},
    {"Свет", "Lx", "усиление", 3, 0xFFE36EFF},
}};

const ElementInfo& info(Element e) { return elements[static_cast<std::size_t>(e)]; }
Color color_of(Element e) { return Color::from_rgba(info(e).color); }

std::uint64_t splitmix(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
float random01(std::uint64_t& s) { return static_cast<float>(splitmix(s) >> 40) / static_cast<float>(1ULL << 24); }
float random_signed(std::uint64_t& s) { return random01(s) * 2.0f - 1.0f; }

std::string subscript(int n) {
    static constexpr std::array<std::string_view, 10> digits{"₀", "₁", "₂", "₃", "₄", "₅", "₆", "₇", "₈", "₉"};
    if (n <= 1) return {};
    std::string out;
    for (const char c : std::to_string(n)) out += digits[static_cast<std::size_t>(c - '0')];
    return out;
}

// =============================================================================
// Компоненты (ZII)
// =============================================================================

struct Body {
    glm::vec2 position{0.0f};
    glm::vec2 previous{0.0f};
    glm::vec2 velocity{0.0f};
};

struct Rune {
    Element element = Element::Ignis;
    std::uint8_t bonds_used = 0;  ///< Сумма кратностей связей.
    bool stable = false;          ///< В насыщенной молекуле.
    bool aromatic = false;        ///< В молекуле с кольцом.
    float energy = 0.0f;
    std::uint32_t molecule = 0;   ///< Номер молекулы в этом тике.
    ECS::Entity cell{};

    [[nodiscard]] int free_valence() const { return static_cast<int>(info(element).valence) - bonds_used; }
};

struct Bond {
    ECS::Entity a{};
    ECS::Entity b{};
    std::uint8_t order = 1;
    float flux = 0.0f; ///< Поток энергии в этом тике — для свечения.
};

struct Cell {
    std::array<glm::vec2, membrane_nodes> nodes{};
    std::array<glm::vec2, membrane_nodes> previous{};
    std::array<glm::vec2, membrane_nodes> velocity{};
    glm::vec2 center{0.0f};
    Element nucleus = Element::Ignis;
    std::uint32_t id = 0;
    int age = 0;
    int division_cooldown = 0;
    float energy = 0.0f;       ///< Сумма энергии рун (считает Metabolism).
    int runes = 0;
    bool casting = false;
    glm::vec2 cast_velocity{0.0f};

    [[nodiscard]] float radius() const {
        float r = 0.0f;
        for (const glm::vec2& n : nodes) r += glm::length(n - center);
        return r / static_cast<float>(membrane_nodes);
    }
};

Cell make_cell(glm::vec2 center, float radius, Element nucleus, std::uint32_t id) {
    Cell c;
    c.center = center;
    c.nucleus = nucleus;
    c.id = id;
    for (int i = 0; i < membrane_nodes; ++i) {
        const float a = 2.0f * pi * static_cast<float>(i) / membrane_nodes;
        c.nodes[static_cast<std::size_t>(i)] = center + glm::vec2{std::cos(a), std::sin(a)} * radius;
    }
    c.previous = c.nodes;
    return c;
}

bool inside(const Cell& c, glm::vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = membrane_nodes - 1; i < membrane_nodes; j = i++) {
        const glm::vec2 a = c.nodes[i];
        const glm::vec2 b = c.nodes[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

// =============================================================================
// События
// =============================================================================

struct InjectRune {
    float x = 0.0f;
    float y = 0.0f;
    std::uint8_t element = 0;
    static constexpr std::string_view event_name = "rune.inject";
    using fields = es::Fields<es::Field<"x", &InjectRune::x>, es::Field<"y", &InjectRune::y>, es::Field<"element", &InjectRune::element>>;
};

/// Связь между рунами: delta = +1 — образовать / повысить кратность, −1 — понизить / разорвать.
struct BondRequest {
    std::uint32_t a_index = 0, a_generation = 0, b_index = 0, b_generation = 0;
    std::int8_t delta = 1;
    [[nodiscard]] ECS::Entity a() const { return {a_index, a_generation}; }
    [[nodiscard]] ECS::Entity b() const { return {b_index, b_generation}; }
    static constexpr std::string_view event_name = "rune.bond_request";
    using fields = es::Fields<es::Field<"a_index", &BondRequest::a_index>, es::Field<"a_generation", &BondRequest::a_generation>,
                              es::Field<"b_index", &BondRequest::b_index>, es::Field<"b_generation", &BondRequest::b_generation>,
                              es::Field<"delta", &BondRequest::delta>>;
};

struct RemoveRune {
    std::uint32_t index = 0, generation = 0;
    static constexpr std::string_view event_name = "rune.remove";
    using fields = es::Fields<es::Field<"index", &RemoveRune::index>, es::Field<"generation", &RemoveRune::generation>>;
};

/// Связь изменилась: order 0 — разорвана; spontaneous — сама (реакция радикалов).
struct BondChanged {
    std::uint8_t order = 0, element_a = 0, element_b = 0, spontaneous = 0;
    static constexpr std::string_view event_name = "rune.bond_changed";
    using fields = es::Fields<es::Field<"order", &BondChanged::order>, es::Field<"element_a", &BondChanged::element_a>,
                              es::Field<"element_b", &BondChanged::element_b>, es::Field<"spontaneous", &BondChanged::spontaneous>>;
};

struct DivisionRequest {
    std::uint32_t index = 0, generation = 0;
    static constexpr std::string_view event_name = "cell.division_request";
    using fields = es::Fields<es::Field<"index", &DivisionRequest::index>, es::Field<"generation", &DivisionRequest::generation>>;
};

struct CellDivided {
    std::uint32_t parent_id = 0, child_id = 0;
    std::uint16_t runes_parent = 0, runes_child = 0;
    static constexpr std::string_view event_name = "cell.divided";
    using fields = es::Fields<es::Field<"parent_id", &CellDivided::parent_id>, es::Field<"child_id", &CellDivided::child_id>,
                              es::Field<"runes_parent", &CellDivided::runes_parent>, es::Field<"runes_child", &CellDivided::runes_child>>;
};

struct CastRequest {
    std::uint32_t index = 0, generation = 0;
    static constexpr std::string_view event_name = "spell.cast_request";
    using fields = es::Fields<es::Field<"index", &CastRequest::index>, es::Field<"generation", &CastRequest::generation>>;
};

struct SpellHit {
    std::uint32_t cell_index = 0, cell_generation = 0;
    float damage = 0.0f;
    float heal = 0.0f;
    float power = 0.0f;
    std::uint32_t id = 0;
    static constexpr std::string_view event_name = "spell.hit";
    using fields = es::Fields<es::Field<"cell_index", &SpellHit::cell_index>, es::Field<"cell_generation", &SpellHit::cell_generation>,
                              es::Field<"damage", &SpellHit::damage>, es::Field<"heal", &SpellHit::heal>, es::Field<"power", &SpellHit::power>,
                              es::Field<"id", &SpellHit::id>>;
};

// =============================================================================
// Анализ заклинания (формула и сила) — чистая функция от клетки
// =============================================================================

struct SpellStats {
    std::array<int, 5> stable_count{};
    std::array<float, 5> stable_energy{};
    int radicals = 0;
    int rings = 0;
    float damage = 0.0f, heal = 0.0f, speed = 0.0f, durability = 0.0f, power = 1.0f;
    std::string formula;
};

SpellStats analyze(ECS::World& world, ECS::Entity cell_entity) {
    SpellStats s;
    // Молекулы клетки: руны по номеру молекулы → состав.
    std::unordered_map<std::uint32_t, std::array<int, 5>> molecules;
    std::unordered_map<std::uint32_t, bool> ring;
    world.view<const Rune>().each([&](const Rune& r) {
        if (r.cell != cell_entity) return;
        if (!r.stable) {
            ++s.radicals;
            return;
        }
        const auto e = static_cast<std::size_t>(r.element);
        ++s.stable_count[e];
        s.stable_energy[e] += r.energy;
        ++molecules[r.molecule][e];
        ring[r.molecule] = ring[r.molecule] || r.aromatic;
    });
    std::vector<std::pair<std::uint32_t, std::array<int, 5>>> sorted(molecules.begin(), molecules.end());
    std::ranges::sort(sorted, {}, [](const auto& m) { return m.first; });
    for (const auto& [id, counts] : sorted) {
        if (!s.formula.empty()) s.formula += " · ";
        for (std::size_t e = 0; e < 5; ++e) {
            if (counts[e] > 0) s.formula += std::string(elements[e].symbol) + subscript(counts[e]);
        }
        if (ring[id]) {
            s.formula += "◯";
            ++s.rings;
        }
    }
    if (s.formula.empty()) s.formula = "—";
    s.power = 1.0f + 0.15f * static_cast<float>(s.stable_count[4]) + 0.5f * static_cast<float>(s.rings);
    s.damage = (s.stable_energy[0] * 0.6f + static_cast<float>(s.stable_count[0]) * 5.0f) * s.power;
    s.heal = (s.stable_energy[1] * 0.5f + static_cast<float>(s.stable_count[1]) * 4.0f) * s.power;
    s.speed = 4.0f + static_cast<float>(s.stable_count[3]) * 1.5f;
    s.durability = static_cast<float>(s.stable_count[2]) * 10.0f;
    return s;
}

// =============================================================================
// Chemistry — единственный владелец структуры: руны, связи, клетки
// =============================================================================

struct Chemistry {
    es::EventReader<InjectRune> injects;
    es::EventReader<BondRequest> bond_requests;
    es::EventReader<RemoveRune> removals;
    es::EventReader<DivisionRequest> divisions;
    es::EventReader<SpellHit> hits;
    es::EventWriter<BondChanged> bond_out;
    es::EventWriter<CellDivided> divided_out;
    std::uint64_t rng = 0xC311;
    std::uint32_t next_cell_id = 1;
    std::size_t reactions = 0, bonds_made = 0, bonds_broken = 0, divisions_done = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Chemistry")
                                    .consumes<InjectRune>()
                                    .consumes<BondRequest>()
                                    .consumes<RemoveRune>()
                                    .consumes<DivisionRequest>()
                                    .consumes<SpellHit>()
                                    .produces<BondChanged>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 512})
                                    .produces<CellDivided>();
        injects = bus.reader<InjectRune>(id);
        bond_requests = bus.reader<BondRequest>(id);
        removals = bus.reader<RemoveRune>(id);
        divisions = bus.reader<DivisionRequest>(id);
        hits = bus.reader<SpellHit>(id);
        bond_out = bus.writer<BondChanged>(id);
        divided_out = bus.writer<CellDivided>(id);
    }

    ECS::Entity add_cell(ECS::World& world, glm::vec2 center, Element nucleus) {
        const ECS::Entity e = world.create();
        world.emplace<Cell>(e, make_cell(center, 75.0f, nucleus, next_cell_id++));
        return e;
    }

    ECS::Entity add_rune(ECS::World& world, ECS::Entity cell, Element element, glm::vec2 at) {
        const ECS::Entity e = world.create();
        world.emplace<Rune>(e, Rune{.element = element, .energy = 5.0f, .cell = cell});
        world.emplace<Body>(e, Body{at, at, {}});
        return e;
    }

    ECS::Entity find_bond(ECS::World& world, ECS::Entity a, ECS::Entity b) const {
        ECS::Entity found{};
        world.view<const Bond>().each([&](ECS::Entity e, const Bond& bond) {
            if ((bond.a == a && bond.b == b) || (bond.a == b && bond.b == a)) found = e;
        });
        return found;
    }

    /// Изменить кратность связи на delta (создать / удалить при необходимости). Валентность проверяется.
    bool change_bond(ECS::World& world, ECS::Entity a, ECS::Entity b, int delta, bool spontaneous) {
        if (a == b || !world.valid(a) || !world.valid(b)) return false;
        Rune* ra = world.get<Rune>(a);
        Rune* rb = world.get<Rune>(b);
        if (ra == nullptr || rb == nullptr || ra->cell != rb->cell) return false;
        const ECS::Entity existing = find_bond(world, a, b);
        Bond* bond = existing ? world.get<Bond>(existing) : nullptr;
        if (delta > 0) {
            if (ra->free_valence() < 1 || rb->free_valence() < 1) return false;
            if (bond != nullptr && bond->order >= 3) return false;
            const float d = glm::length(world.get<Body>(a)->position - world.get<Body>(b)->position);
            if (d > 90.0f) return false;
            if (bond == nullptr) {
                const ECS::Entity e = world.create();
                world.emplace<Bond>(e, Bond{a, b, 1, 0.0f});
                bond = world.get<Bond>(e);
            } else {
                ++bond->order;
            }
            ++ra->bonds_used, ++rb->bonds_used;
            ++bonds_made;
            bond_out.emit({bond->order, static_cast<std::uint8_t>(ra->element), static_cast<std::uint8_t>(rb->element), spontaneous ? std::uint8_t{1} : std::uint8_t{0}});
            return true;
        }
        if (bond == nullptr) return false;
        --ra->bonds_used, --rb->bonds_used;
        ++bonds_broken;
        const auto order = static_cast<std::uint8_t>(bond->order - 1);
        bond_out.emit({order, static_cast<std::uint8_t>(ra->element), static_cast<std::uint8_t>(rb->element), 0});
        if (order == 0) {
            world.destroy(existing);
        } else {
            bond->order = order;
        }
        return true;
    }

    void remove_rune(ECS::World& world, ECS::Entity rune) {
        std::vector<ECS::Entity> bonds;
        world.view<const Bond>().each([&](ECS::Entity e, const Bond& b) {
            if (b.a == rune || b.b == rune) bonds.push_back(e);
        });
        for (const ECS::Entity e : bonds) {
            const Bond b = *world.get<Bond>(e);
            for (int i = 0; i < b.order; ++i) change_bond(world, b.a, b.b, -1, false);
        }
        world.destroy(rune);
    }

    void destroy_cell(ECS::World& world, ECS::Entity cell) {
        std::vector<ECS::Entity> runes;
        world.view<const Rune>().each([&](ECS::Entity e, const Rune& r) {
            if (r.cell == cell) runes.push_back(e);
        });
        for (const ECS::Entity r : runes) remove_rune(world, r);
        world.destroy(cell);
    }

    /// Митоз: разрезать клетку прямой через центр, руны разойтись по сторонам, разрезанные связи порвать.
    void divide(ECS::World& world, ECS::Entity parent_entity) {
        if (world.count<Cell>() >= max_cells) return;
        Cell parent = *world.get<Cell>(parent_entity);
        const float angle = random01(rng) * 2.0f * pi;
        const glm::vec2 axis{std::cos(angle), std::sin(angle)};
        const float radius = parent.radius() / std::sqrt(2.0f);
        const ECS::Entity child_entity = world.create();
        world.emplace<Cell>(child_entity, make_cell(parent.center + axis * radius * 0.9f, radius, parent.nucleus, next_cell_id++));
        Cell& p = *world.get<Cell>(parent_entity);
        const std::uint32_t parent_id = p.id;
        p = make_cell(parent.center - axis * radius * 0.9f, radius, parent.nucleus, parent.id);
        p.division_cooldown = world.get<Cell>(child_entity)->division_cooldown = 900;

        std::vector<ECS::Entity> moved;
        std::uint16_t stay = 0, go = 0;
        world.view<Rune, Body>().each([&](ECS::Entity e, Rune& r, Body& b) {
            if (r.cell != parent_entity) return;
            const bool to_child = glm::dot(b.position - parent.center, axis) > 0.0f;
            const glm::vec2 shift = axis * radius * 0.9f * (to_child ? 1.0f : -1.0f);
            b.position += shift, b.previous += shift;
            if (to_child) {
                r.cell = child_entity;
                moved.push_back(e);
                ++go;
            } else {
                ++stay;
            }
        });
        std::vector<ECS::Entity> cut;
        world.view<const Bond>().each([&](ECS::Entity e, const Bond& b) {
            const Rune* ra = world.get<Rune>(b.a);
            const Rune* rb = world.get<Rune>(b.b);
            if (ra != nullptr && rb != nullptr && ra->cell != rb->cell) cut.push_back(e);
        });
        for (const ECS::Entity e : cut) {
            const Bond b = *world.get<Bond>(e);
            // Руны уже в разных клетках — change_bond откажет; рвём напрямую.
            world.get<Rune>(b.a)->bonds_used = static_cast<std::uint8_t>(world.get<Rune>(b.a)->bonds_used - b.order);
            world.get<Rune>(b.b)->bonds_used = static_cast<std::uint8_t>(world.get<Rune>(b.b)->bonds_used - b.order);
            bond_out.emit({0, static_cast<std::uint8_t>(world.get<Rune>(b.a)->element), static_cast<std::uint8_t>(world.get<Rune>(b.b)->element), 0});
            world.destroy(e);
            ++bonds_broken;
        }
        ++divisions_done;
        divided_out.emit({parent_id, world.get<Cell>(child_entity)->id, stay, go});
    }

    /// Радикалы рядом в одной клетке сами образуют связь (с небольшой вероятностью за тик).
    void react(ECS::World& world) {
        std::vector<std::pair<ECS::Entity, glm::vec2>> radicals;
        world.view<const Rune, const Body>().each([&](ECS::Entity e, const Rune& r, const Body& b) {
            if (r.free_valence() > 0) radicals.emplace_back(e, b.position);
        });
        std::ranges::sort(radicals, {}, [](const auto& r) { return r.first.index; }); // порядок не зависит от раскладки пула
        for (std::size_t i = 0; i < radicals.size(); ++i) {
            for (std::size_t j = i + 1; j < radicals.size(); ++j) {
                if (glm::length(radicals[i].second - radicals[j].second) > 2.0f * rune_radius + 8.0f) continue;
                if (random01(rng) > 0.02f) continue;
                if (change_bond(world, radicals[i].first, radicals[j].first, +1, true)) ++reactions;
            }
        }
    }

    /// Молекулы (union-find по связям), насыщенность и кольца.
    void classify(ECS::World& world) {
        std::unordered_map<std::uint32_t, std::uint32_t> parent;
        std::vector<ECS::Entity> runes;
        world.view<const Rune>().each([&](ECS::Entity e, const Rune&) {
            parent[e.index] = e.index;
            runes.push_back(e);
        });
        const auto find = [&](std::uint32_t x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        std::unordered_map<std::uint32_t, bool> cyclic;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
        world.view<const Bond>().each([&](const Bond& b) { edges.emplace_back(b.a.index, b.b.index); });
        std::ranges::sort(edges);
        for (const auto& [a, b] : edges) {
            const std::uint32_t ra = find(a), rb = find(b);
            if (ra == rb) {
                cyclic[ra] = true; // ребро внутри уже связной компоненты — цикл
            } else {
                parent[ra] = rb;
                if (cyclic[ra]) cyclic[rb] = true;
            }
        }
        std::unordered_map<std::uint32_t, bool> saturated;
        for (const ECS::Entity e : runes) {
            const Rune& r = *world.get<Rune>(e);
            const std::uint32_t root = find(e.index);
            if (!saturated.contains(root)) saturated[root] = true;
            if (r.free_valence() != 0) saturated[root] = false;
        }
        for (const ECS::Entity e : runes) {
            Rune& r = *world.get<Rune>(e);
            const std::uint32_t root = find(e.index);
            r.molecule = root;
            r.stable = saturated[root] && r.bonds_used > 0;
            r.aromatic = cyclic[find(root)];
        }
    }

    void tick(ECS::World& world) {
        for (const InjectRune& in : injects.events()) {
            const glm::vec2 at{in.x, in.y};
            ECS::Entity target{};
            world.view<const Cell>().each([&](ECS::Entity e, const Cell& c) {
                if (!c.casting && inside(c, at)) target = e;
            });
            if (!target || world.get<Cell>(target)->runes >= static_cast<int>(max_runes_per_cell)) continue;
            add_rune(world, target, static_cast<Element>(std::min<std::uint8_t>(in.element, 4)), at);
        }
        for (const BondRequest& r : bond_requests.events()) change_bond(world, r.a(), r.b(), r.delta, false);
        for (const RemoveRune& r : removals.events()) {
            if (world.valid({r.index, r.generation}) && world.get<Rune>({r.index, r.generation}) != nullptr) remove_rune(world, {r.index, r.generation});
        }
        for (const DivisionRequest& d : divisions.events()) {
            if (world.valid({d.index, d.generation})) divide(world, {d.index, d.generation});
        }
        for (const SpellHit& h : hits.events()) {
            if (world.valid({h.cell_index, h.cell_generation})) destroy_cell(world, {h.cell_index, h.cell_generation});
        }
        react(world);
        classify(world);
    }
};

// =============================================================================
// Physics — мягкие мембраны и руны; параллельно по клеткам (JobSystem)
// =============================================================================

struct CellWork {
    ECS::Entity entity;
    Cell* cell = nullptr;
    std::span<ECS::Entity> runes;
    std::span<ECS::Entity> bonds;
};

struct Physics {
    std::vector<CellWork> work;
    double last_ms = 0.0;

    /// Списки рун и связей по клеткам — в памяти тика (живут до следующего тика).
    void gather(ECS::World& world, ms::Arena& arena) {
        work.clear();
        std::unordered_map<std::uint32_t, std::size_t> slot;
        world.view<Cell>().each([&](ECS::Entity e, Cell& c) {
            slot[e.index] = work.size();
            work.push_back({e, &c, {}, {}});
        });
        std::ranges::sort(work, {}, [](const CellWork& w) { return w.cell->id; });
        for (std::size_t i = 0; i < work.size(); ++i) slot[work[i].entity.index] = i;
        std::vector<int> rune_count(work.size(), 0), bond_count(work.size(), 0);
        world.view<const Rune>().each([&](const Rune& r) { ++rune_count[slot[r.cell.index]]; });
        world.view<const Bond>().each([&](const Bond& b) { ++bond_count[slot[world.get<Rune>(b.a)->cell.index]]; });
        for (std::size_t i = 0; i < work.size(); ++i) {
            work[i].runes = arena.push_array<ECS::Entity>(static_cast<std::size_t>(rune_count[i]));
            work[i].bonds = arena.push_array<ECS::Entity>(static_cast<std::size_t>(bond_count[i]));
            work[i].cell->runes = rune_count[i];
            rune_count[i] = bond_count[i] = 0;
        }
        std::vector<std::pair<std::uint32_t, ECS::Entity>> runes;
        world.view<const Rune>().each([&](ECS::Entity e, const Rune& r) { runes.emplace_back(r.cell.index, e); });
        std::ranges::sort(runes, {}, [](const auto& p) { return p.second.index; });
        for (const auto& [cell, e] : runes) {
            const std::size_t s = slot[cell];
            work[s].runes[static_cast<std::size_t>(rune_count[s]++)] = e;
        }
        std::vector<ECS::Entity> bonds;
        world.view<const Bond>().each([&](ECS::Entity e, const Bond&) { bonds.push_back(e); });
        std::ranges::sort(bonds, {}, &ECS::Entity::index);
        for (const ECS::Entity e : bonds) {
            const std::size_t s = slot[world.get<Rune>(world.get<Bond>(e)->a)->cell.index];
            work[s].bonds[static_cast<std::size_t>(bond_count[s]++)] = e;
        }
    }

    static void step_cell(ECS::World& world, CellWork& w, std::uint64_t seed) {
        Cell& c = *w.cell;
        std::uint64_t rng = seed;
        // --- мембрана: пружины, изгиб, давление к «площади покоя», которая растёт с числом рун
        float area = 0.0f;
        for (std::size_t i = 0; i < membrane_nodes; ++i) {
            const glm::vec2 a = c.nodes[i], b = c.nodes[(i + 1) % membrane_nodes];
            area += a.x * b.y - b.x * a.y;
        }
        area = std::abs(area) * 0.5f;
        const float target_radius = 62.0f + 10.0f * std::sqrt(static_cast<float>(w.runes.size()) + 1.0f);
        const float rest_area = pi * target_radius * target_radius;
        const float pressure = std::clamp((rest_area - area) / rest_area, -0.5f, 0.5f) * 3.0f;
        float perimeter = 2.0f * pi * target_radius;
        const float rest = perimeter / membrane_nodes;
        int terra = 0;
        for (const ECS::Entity r : w.runes) terra += world.get<Rune>(r)->element == Element::Terra ? 1 : 0;
        const float stiffness = 0.25f + 0.02f * static_cast<float>(std::min(terra, 10)); // Земля — жёсткая оболочка
        c.previous = c.nodes;
        glm::vec2 centroid{0.0f};
        for (const glm::vec2& n : c.nodes) centroid += n;
        centroid /= static_cast<float>(membrane_nodes);
        for (std::size_t i = 0; i < membrane_nodes; ++i) {
            const glm::vec2 p = c.nodes[i];
            const glm::vec2 prev = c.nodes[(i + membrane_nodes - 1) % membrane_nodes];
            const glm::vec2 next = c.nodes[(i + 1) % membrane_nodes];
            glm::vec2 force{0.0f};
            for (const glm::vec2 q : {prev, next}) {
                const glm::vec2 d = q - p;
                const float len = std::max(glm::length(d), 1e-3f);
                force += d / len * (len - rest) * stiffness;
            }
            force += (prev + next - 2.0f * p) * 0.08f;
            glm::vec2 normal = glm::normalize(glm::vec2{next.y - prev.y, prev.x - next.x});
            if (glm::dot(normal, p - centroid) < 0.0f) normal = -normal;
            force += normal * pressure;
            force += glm::vec2{random_signed(rng), random_signed(rng)} * 0.05f; // «дыхание» живой оболочки
            const glm::vec2 to_dish = p - dish_center;
            if (!c.casting && glm::length(to_dish) > dish_radius - 6.0f) force -= glm::normalize(to_dish) * 1.5f;
            c.velocity[i] = (c.velocity[i] + force) * 0.82f;
        }
        for (std::size_t i = 0; i < membrane_nodes; ++i) c.nodes[i] += c.velocity[i] + c.cast_velocity;
        c.center = centroid + c.cast_velocity;

        // --- руны: броуновское движение, связи-пружины, отталкивание, ядро, мембрана
        for (const ECS::Entity e : w.runes) {
            Body& b = *world.get<Body>(e);
            b.previous = b.position;
            b.velocity += glm::vec2{random_signed(rng), random_signed(rng)} * 0.25f;
        }
        for (const ECS::Entity e : w.bonds) {
            const Bond& bond = *world.get<Bond>(e);
            Body& a = *world.get<Body>(bond.a);
            Body& b = *world.get<Body>(bond.b);
            const glm::vec2 d = b.position - a.position;
            const float len = std::max(glm::length(d), 1e-3f);
            const float rest_len = bond.order == 1 ? 42.0f : bond.order == 2 ? 38.0f : 35.0f; // кратнее — короче, как в химии
            const glm::vec2 f = d / len * (len - rest_len) * 0.12f;
            a.velocity += f;
            b.velocity -= f;
        }
        for (std::size_t i = 0; i < w.runes.size(); ++i) {
            Body& a = *world.get<Body>(w.runes[i]);
            for (std::size_t j = i + 1; j < w.runes.size(); ++j) {
                Body& b = *world.get<Body>(w.runes[j]);
                const glm::vec2 d = b.position - a.position;
                const float len = glm::length(d);
                if (len < 2.0f * rune_radius + 2.0f && len > 1e-3f) {
                    const glm::vec2 push = d / len * (2.0f * rune_radius + 2.0f - len) * 0.25f;
                    a.velocity -= push;
                    b.velocity += push;
                }
            }
            const glm::vec2 from_core = a.position - c.center;
            const float core = glm::length(from_core);
            if (core < nucleus_radius + rune_radius && core > 1e-3f) a.velocity += from_core / core * 0.8f;
        }
        for (const ECS::Entity e : w.runes) {
            Body& b = *world.get<Body>(e);
            b.velocity *= 0.85f;
            b.position += b.velocity + c.cast_velocity;
            if (!inside(c, b.position)) { // упёрлась в мембрану: назад, а мембрану — наружу
                const glm::vec2 inward = glm::normalize(c.center - b.position);
                b.position += inward * 4.0f;
                b.velocity = inward * 0.5f;
                std::size_t nearest = 0;
                float best = std::numeric_limits<float>::max();
                for (std::size_t i = 0; i < membrane_nodes; ++i) {
                    const float d = glm::length(c.nodes[i] - b.position);
                    if (d < best) best = d, nearest = i;
                }
                c.velocity[nearest] -= inward * 0.6f;
            }
        }
    }

    void tick(ECS::World& world, js::Scheduler& jobs, ms::Arena& arena, es::Tick now) {
        gather(world, arena);
        const auto start = std::chrono::steady_clock::now();
        js::parallel_for(jobs, work.size(), 1, [&](std::size_t begin, std::size_t end, std::size_t) {
            for (std::size_t i = begin; i < end; ++i) {
                step_cell(world, work[i], (static_cast<std::uint64_t>(now) << 20) ^ work[i].cell->id * 0x9E3779B97F4A7C15ULL);
            }
        });
        // Клетки расталкивают друг друга (последовательно: пар мало).
        for (std::size_t i = 0; i < work.size(); ++i) {
            for (std::size_t j = i + 1; j < work.size(); ++j) {
                Cell& a = *work[i].cell;
                Cell& b = *work[j].cell;
                const float ra = a.radius(), rb = b.radius();
                const glm::vec2 d = b.center - a.center;
                const float len = glm::length(d);
                if (len >= ra + rb || len < 1e-3f) continue;
                // Перекрытие убираем сразу наполовину: узлы и руны обеих клеток сдвигаются целиком.
                const glm::vec2 push = d / len * (ra + rb - len) * 0.25f;
                for (glm::vec2& n : a.nodes) n -= push;
                for (glm::vec2& n : b.nodes) n += push;
                for (const ECS::Entity r : work[i].runes) world.get<Body>(r)->position -= push;
                for (const ECS::Entity r : work[j].runes) world.get<Body>(r)->position += push;
                a.center -= push;
                b.center += push;
            }
        }
        last_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
};

// =============================================================================
// Metabolism — частицы маны (Pool + JobSystem), энергия рун, сигнал к делению
// =============================================================================

struct Mote {
    glm::vec2 position{0.0f};
    glm::vec2 previous{0.0f};
    glm::vec2 velocity{0.0f};
    std::uint32_t cell_index = 0, cell_generation = 0;
    int life = 0;
};

struct Absorbed {
    std::uint32_t mote = 0;
    ECS::Entity rune{};
};

struct Metabolism {
    es::EventWriter<DivisionRequest> division_out;
    ms::Pool<Mote> pool = ms::Pool<Mote>::reserve(32768, ms::KiB(64), ms::MemoryTag::Game);
    std::vector<Mote*> motes;
    js::ChunkBuffers<Absorbed> absorbed;
    std::size_t spawned = 0, eaten = 0, requested = 0;
    double last_ms = 0.0;

    void declare(es::EventBus& bus) { division_out = bus.writer<DivisionRequest>(bus.declare_module("Metabolism").produces<DivisionRequest>()); }

    void tick(ECS::World& world, const Physics& physics, js::Scheduler& jobs, es::Tick now) {
        const auto start = std::chrono::steady_clock::now();
        // Ядра выпускают ману.
        for (const CellWork& w : physics.work) {
            if (w.cell->casting || now % 5 != w.cell->id % 5) continue;
            std::uint64_t rng = now * 7919ULL + w.cell->id;
            Mote* m = pool.allocate();
            if (m == nullptr) break;
            const float a = random01(rng) * 2.0f * pi;
            *m = Mote{w.cell->center, w.cell->center, glm::vec2{std::cos(a), std::sin(a)} * 1.4f, w.entity.index, w.entity.generation, 420};
            motes.push_back(m);
            ++spawned;
        }

        // Движение и поглощение — параллельно; кусок пишет только свои частицы и свой буфер поглощений.
        std::unordered_map<std::uint32_t, const CellWork*> by_cell;
        for (const CellWork& w : physics.work) by_cell[w.entity.index] = &w;
        constexpr std::size_t grain = 256;
        absorbed.reset(js::chunk_count(motes.size(), grain));
        js::parallel_for(jobs, motes.size(), grain, [&](std::size_t begin, std::size_t end, std::size_t chunk) {
            for (std::size_t i = begin; i < end; ++i) {
                Mote& m = *motes[i];
                const auto it = by_cell.find(m.cell_index);
                if (it == by_cell.end() || it->second->entity.generation != m.cell_generation) {
                    m.life = 0;
                    continue;
                }
                const CellWork& w = *it->second;
                std::uint64_t rng = (static_cast<std::uint64_t>(now) << 24) ^ (i * 0x9E3779B97F4A7C15ULL);
                m.previous = m.position;
                m.velocity = (m.velocity + glm::vec2{random_signed(rng), random_signed(rng)} * 0.3f) * 0.96f;
                m.position += m.velocity + w.cell->cast_velocity;
                if (!inside(*w.cell, m.position)) {
                    m.velocity = glm::normalize(w.cell->center - m.position) * 1.2f;
                    m.position = m.previous;
                }
                --m.life;
                for (const ECS::Entity r : w.runes) {
                    const Rune& rune = *world.get<Rune>(r);
                    if (rune.stable && glm::length(world.get<Body>(r)->position - m.position) < rune_radius + 2.0f) {
                        absorbed[chunk].push_back({static_cast<std::uint32_t>(i), r});
                        break;
                    }
                }
            }
        });
        absorbed.for_each([&](const Absorbed& a) { // по порядку кусков — детерминированно
            Mote& m = *motes[a.mote];
            if (m.life <= 0) return;
            world.get<Rune>(a.rune)->energy = std::min(world.get<Rune>(a.rune)->energy + 1.0f, 100.0f);
            m.life = 0;
            ++eaten;
        });
        std::size_t kept = 0;
        for (Mote* m : motes) {
            if (m->life > 0) {
                motes[kept++] = m;
            } else {
                pool.free(m);
            }
        }
        motes.resize(kept);

        // Энергия: поток по связям (по снимку — порядок не важен), утечка у радикалов, расход.
        std::unordered_map<std::uint32_t, float> before;
        world.view<const Rune>().each([&](ECS::Entity e, const Rune& r) { before[e.index] = r.energy; });
        world.view<Bond>().each([&](Bond& b) {
            const float flux = 0.04f * static_cast<float>(b.order) * (before[b.a.index] - before[b.b.index]);
            world.get<Rune>(b.a)->energy -= flux;
            world.get<Rune>(b.b)->energy += flux;
            b.flux = std::abs(flux);
        });
        world.view<Rune>().each([&](Rune& r) {
            r.energy = std::max(0.0f, r.energy - (r.stable ? 0.004f : 0.03f));
        });

        // Энергия клеток и деление.
        std::unordered_map<std::uint32_t, float> energy;
        world.view<const Rune>().each([&](const Rune& r) { energy[r.cell.index] += r.energy; });
        for (const CellWork& w : physics.work) {
            Cell& c = *w.cell;
            c.energy = energy[w.entity.index];
            ++c.age;
            if (c.division_cooldown > 0) --c.division_cooldown;
            const float threshold = 40.0f * std::sqrt(static_cast<float>(std::max(c.runes, 1)));
            if (!c.casting && c.runes >= 8 && c.division_cooldown == 0 && c.energy > threshold) {
                division_out.emit({w.entity.index, w.entity.generation});
                c.division_cooldown = 900;
                ++requested;
            }
        }
        last_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void clear() {
        for (Mote* m : motes) pool.free(m);
        motes.clear();
    }
};

// =============================================================================
// Caster и голем
// =============================================================================

struct Caster {
    es::EventReader<CastRequest> requests;
    es::EventWriter<SpellHit> hit_out;
    std::size_t casts = 0;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Caster").consumes<CastRequest>().produces<SpellHit>();
        requests = bus.reader<CastRequest>(id);
        hit_out = bus.writer<SpellHit>(id);
    }

    void tick(ECS::World& world) {
        for (const CastRequest& r : requests.events()) {
            Cell* c = world.valid({r.index, r.generation}) ? world.get<Cell>({r.index, r.generation}) : nullptr;
            if (c == nullptr || c->casting) continue;
            const SpellStats stats = analyze(world, {r.index, r.generation});
            c->casting = true;
            c->cast_velocity = glm::normalize(golem_center - c->center) * stats.speed;
            ++casts;
        }
        world.view<const Cell>().each([&](ECS::Entity e, const Cell& c) {
            if (!c.casting) return;
            if (glm::length(c.center - golem_center) < golem_radius + c.radius() * 0.5f || c.center.x > world_size.x + 200.0f) {
                const SpellStats stats = analyze(world, e);
                hit_out.emit({e.index, e.generation, stats.damage, stats.heal, stats.power, c.id});
            }
        });
    }
};

struct Golem {
    es::EventReader<SpellHit> hits;
    float health = 500.0f;
    float healed = 0.0f;
    float total_damage = 0.0f;
    int shake = 0;
    struct Number {
        float amount;
        int life;
        bool heal;
    };
    std::vector<Number> numbers;

    void declare(es::EventBus& bus) { hits = bus.reader<SpellHit>(bus.declare_module("Golem").consumes<SpellHit>()); }

    void tick() {
        for (const SpellHit& h : hits.events()) {
            health = std::max(0.0f, health - h.damage);
            healed += h.heal;
            total_damage += h.damage;
            shake = 20;
            numbers.push_back({h.damage, 80, false});
            if (h.heal > 0.0f) numbers.push_back({h.heal, 80, true});
        }
        if (health <= 0.0f) health = 500.0f; // голем собирается заново
        health = std::min(500.0f, health + 0.05f);
        if (shake > 0) --shake;
        for (Number& n : numbers) --n.life;
        std::erase_if(numbers, [](const Number& n) { return n.life <= 0; });
    }
};

// =============================================================================
// Chronicle — что происходит в «пробирке»
// =============================================================================

struct Chronicle {
    es::EventReader<BondChanged> bonds;
    es::EventReader<CellDivided> divided;
    es::EventReader<SpellHit> hits;
    std::deque<std::string> lines;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Chronicle").consumes<BondChanged>().consumes<CellDivided>().consumes<SpellHit>();
        bonds = bus.reader<BondChanged>(id);
        divided = bus.reader<CellDivided>(id);
        hits = bus.reader<SpellHit>(id);
    }

    void add(std::string line) {
        lines.push_back(std::move(line));
        while (lines.size() > 8) lines.pop_front();
    }

    void tick() {
        static constexpr std::array<std::string_view, 4> order_mark{"×", "—", "=", "≡"};
        for (const BondChanged& b : bonds.events()) {
            const std::string_view a = elements[b.element_a].symbol, c = elements[b.element_b].symbol;
            if (b.order == 0) add(std::format("связь {}–{} разорвана", a, c));
            else if (b.spontaneous) add(std::format("реакция: {}{}{}", a, order_mark[b.order], c));
            else add(std::format("связь {}{}{}", a, order_mark[b.order], c));
        }
        for (const CellDivided& d : divided.events()) add(std::format("клетка {} делится: {} + {} рун (дочь {})", d.parent_id, d.runes_parent, d.runes_child, d.child_id));
        for (const SpellHit& h : hits.events()) add(std::format("заклинание {}: {:.0f} урона, {:.0f} лечения (×{:.2f})", h.id, h.damage, h.heal, h.power));
    }
};

// =============================================================================
// Ввод игрока и автоигра: оба пишут в шину
// =============================================================================

struct PlayerInput {
    es::EventReader<Core::KeyEvent> keys;
    es::EventReader<Core::MouseButtonEvent> clicks;
    es::EventWriter<InjectRune> inject_out;
    es::EventWriter<BondRequest> bond_out;
    es::EventWriter<RemoveRune> remove_out;
    es::EventWriter<CastRequest> cast_out;
    Element selected = Element::Ignis;
    ECS::Entity drag_from{};
    glm::vec2 drag_start{0.0f};
    glm::vec2 pointer{0.0f};
    bool autoplay = false;
    bool new_cell = false;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("PlayerInput")
                                    .consumes<Core::KeyEvent>()
                                    .consumes<Core::MouseButtonEvent>()
                                    .produces<InjectRune>()
                                    .produces<BondRequest>()
                                    .produces<RemoveRune>()
                                    .produces<CastRequest>();
        keys = bus.reader<Core::KeyEvent>(id);
        clicks = bus.reader<Core::MouseButtonEvent>(id);
        inject_out = bus.writer<InjectRune>(id);
        bond_out = bus.writer<BondRequest>(id);
        remove_out = bus.writer<RemoveRune>(id);
        cast_out = bus.writer<CastRequest>(id);
    }

    static ECS::Entity rune_at(ECS::World& world, glm::vec2 p) {
        ECS::Entity found{};
        float best = rune_radius + 4.0f;
        world.view<const Rune, const Body>().each([&](ECS::Entity e, const Rune&, const Body& b) {
            const float d = glm::length(b.position - p);
            if (d < best) best = d, found = e;
        });
        return found;
    }

    static ECS::Entity cell_at(ECS::World& world, glm::vec2 p) {
        ECS::Entity found{};
        world.view<const Cell>().each([&](ECS::Entity e, const Cell& c) {
            if (!c.casting && inside(c, p)) found = e;
        });
        return found;
    }

    static BondRequest request(ECS::Entity a, ECS::Entity b, std::int8_t delta) {
        return {a.index, a.generation, b.index, b.generation, delta};
    }

    void tick(ECS::World& world) {
        for (const Core::KeyEvent& k : keys.events()) {
            if (!k.pressed()) continue;
            if (const int digit = InputSystem::digit_value(k.code()); digit >= 1 && digit <= 5) selected = static_cast<Element>(digit - 1);
            if (k.code() == Key::A) autoplay = !autoplay;
            if (k.code() == Key::N) new_cell = true;
            if (k.code() == Key::Space) {
                if (const ECS::Entity c = cell_at(world, pointer)) cast_out.emit({c.index, c.generation});
            }
            if (k.code() == Key::X) {
                if (const ECS::Entity r = rune_at(world, pointer)) remove_out.emit({r.index, r.generation});
            }
        }
        for (const Core::MouseButtonEvent& m : clicks.events()) {
            const glm::vec2 at{m.world_x, m.world_y};
            if (m.which() == MouseButton::Left && m.pressed()) {
                inject_out.emit({at.x, at.y, static_cast<std::uint8_t>(selected)});
            } else if (m.which() == MouseButton::Right && m.pressed()) {
                drag_from = rune_at(world, at);
                drag_start = at;
            } else if (m.which() == MouseButton::Right && m.released() && drag_from) {
                const ECS::Entity to = rune_at(world, at);
                if (to && to != drag_from) {
                    bond_out.emit(request(drag_from, to, +1));
                } else if (glm::length(at - drag_start) < 6.0f) { // щелчок: ослабить самую слабую связь руны
                    ECS::Entity other{};
                    int weakest = 4;
                    world.view<const Bond>().each([&](const Bond& b) {
                        if ((b.a == drag_from || b.b == drag_from) && b.order < weakest) {
                            weakest = b.order;
                            other = b.a == drag_from ? b.b : b.a;
                        }
                    });
                    if (other) bond_out.emit(request(drag_from, other, -1));
                }
                drag_from = {};
            }
        }
    }
};

/// Автоигра: «лаборант» сажает руны, соединяет радикалы и иногда запускает клетку.
struct Autoplay {
    es::EventWriter<InjectRune> inject_out;
    es::EventWriter<BondRequest> bond_out;
    es::EventWriter<CastRequest> cast_out;
    std::uint64_t rng = 0xA11CE;

    void declare(es::EventBus& bus) {
        const es::ModuleId id = bus.declare_module("Autoplay").produces<InjectRune>().produces<BondRequest>().produces<CastRequest>();
        inject_out = bus.writer<InjectRune>(id);
        bond_out = bus.writer<BondRequest>(id);
        cast_out = bus.writer<CastRequest>(id);
    }

    void tick(ECS::World& world, const Physics& physics, es::Tick now) {
        if (physics.work.empty()) return;
        const CellWork& w = physics.work[static_cast<std::size_t>(now / 30) % physics.work.size()];
        if (w.cell->casting) return;
        if (now % 30 == 0 && w.runes.size() < 22) {
            const float a = random01(rng) * 2.0f * pi;
            const glm::vec2 at = w.cell->center + glm::vec2{std::cos(a), std::sin(a)} * w.cell->radius() * (0.35f + 0.3f * random01(rng));
            inject_out.emit({at.x, at.y, static_cast<std::uint8_t>(splitmix(rng) % 5)});
        }
        if (now % 12 == 6) { // соединить два близких радикала
            for (std::size_t i = 0; i < w.runes.size(); ++i) {
                const Rune& a = *world.get<Rune>(w.runes[i]);
                if (a.free_valence() <= 0) continue;
                for (std::size_t j = i + 1; j < w.runes.size(); ++j) {
                    const Rune& b = *world.get<Rune>(w.runes[j]);
                    if (b.free_valence() <= 0) continue;
                    if (glm::length(world.get<Body>(w.runes[i])->position - world.get<Body>(w.runes[j])->position) < 70.0f) {
                        bond_out.emit(PlayerInput::request(w.runes[i], w.runes[j], +1));
                        return;
                    }
                }
            }
        }
        if (now % 700 == 699 && physics.work.size() > 1) cast_out.emit({w.entity.index, w.entity.generation});
    }
};

// =============================================================================
// Игра
// =============================================================================

class RuneCell final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return ::world_size; }

    void setup(Core::App& app) override {
        const auto& args = app.config().extra_args;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--autoplay") input.autoplay = true;
            if (args[i] == "--seed" && i + 1 < args.size()) chemistry.rng = std::strtoull(args[i + 1].c_str(), nullptr, 10);
        }
        es::EventBus& bus = app.bus();
        input.declare(bus);
        autoplay.declare(bus);
        chemistry.declare(bus);
        metabolism.declare(bus);
        caster.declare(bus);
        golem.declare(bus);
        chronicle.declare(bus);

        Renderer2D& r = app.renderer();
        circle = r.create_texture(Procedural::circle_image(64, Colors::white), {.filter = TextureFilter::Linear});
        ring = r.create_texture(Procedural::circle_image(64, Colors::white, 5.0f), {.filter = TextureFilter::Linear});
        Image agar = Procedural::noise_image({.width = 512, .height = 512, .scale = 6.0f, .z = 3.3f, .octaves = 4},
                                             {{0.0f, Color::from_rgba(0x3A3122FF)}, {0.5f, Color::from_rgba(0x5C4D33FF)}, {1.0f, Color::from_rgba(0x7A6A48FF)}});
        const Image mask = Procedural::circle_image(512, Colors::white);
        for (std::size_t i = 0; i < agar.pixels().size(); ++i) agar.pixels()[i].a = mask.pixels()[i].a;
        dish = r.create_texture(agar, {.filter = TextureFilter::Linear});

        seed_cells();
        std::println("RuneCell: {} cells, {} runes; backend {}", world.count<Cell>(), world.count<Rune>(), to_string(app.device().backend()));
    }

    void tick(Core::App& app) override {
        const es::Tick now = app.tick();
        input.pointer = app.input().mouse_world;
        input.tick(world);
        if (input.new_cell) {
            input.new_cell = false;
            add_free_cell();
        }
        chemistry.tick(world);
        physics.tick(world, app.jobs(), app.tick_arena(), now);
        metabolism.tick(world, physics, app.jobs(), now);
        caster.tick(world);
        golem.tick();
        chronicle.tick();
        if (input.autoplay) {
            autoplay.tick(world, physics, now);
            if (now % 1500 == 1499 && world.count<Cell>() < 3) add_free_cell();
        }
    }

    void render(Core::App& app, Renderer2D& r) override {
        const float alpha = app.tick_alpha();
        r.draw(SpriteInstance{.position = dish_center, .size = glm::vec2{dish_radius * 2.0f + 10.0f}, .color = Color{255, 255, 255, 255}, .texture = dish, .layer = -20});
        r.draw(SpriteInstance{.position = dish_center, .size = glm::vec2{dish_radius * 2.0f + 22.0f}, .color = Color{200, 220, 230, 120}, .texture = ring, .layer = -19});

        // Клетки: цитопазма (лучи от центра к узлам), мембрана, ядро.
        hovered_cell = {};
        world.view<const Cell>().each([&](ECS::Entity e, const Cell& c) {
            std::array<glm::vec2, membrane_nodes> nodes{};
            for (std::size_t i = 0; i < membrane_nodes; ++i) nodes[i] = glm::mix(c.previous[i], c.nodes[i], alpha);
            glm::vec2 center{0.0f};
            for (const glm::vec2& n : nodes) center += n;
            center /= static_cast<float>(membrane_nodes);
            const Color tint = color_of(c.nucleus);
            const Color plasma = Color::lerp(Color::from_rgba(0x1A2420FF), tint, 0.18f).with_alpha(235);
            for (std::size_t i = 0; i < membrane_nodes; ++i) {
                const glm::vec2 a = nodes[i], b = nodes[(i + 1) % membrane_nodes];
                const glm::vec2 mid = (a + b) * 0.5f;
                r.draw_line(center, mid, glm::length(b - a) * 1.25f + 2.0f, plasma, 0);
            }
            const bool hover = inside(c, input.pointer);
            if (hover) hovered_cell = e;
            const Color membrane = Color::lerp(tint, Colors::white, 0.45f).with_alpha(hover ? 255 : 200);
            for (std::size_t i = 0; i < membrane_nodes; ++i) {
                r.draw_line(nodes[i], nodes[(i + 1) % membrane_nodes], hover ? 6.0f : 4.5f, membrane, 3);
            }
            const float pulse = 1.0f + 0.08f * std::sin(static_cast<float>(c.age) * 0.08f);
            r.draw(SpriteInstance{.position = center, .size = glm::vec2{nucleus_radius * 2.2f * pulse}, .color = tint.scaled(0.8f), .texture = circle, .layer = 4});
            r.draw(SpriteInstance{.position = center, .size = glm::vec2{nucleus_radius * 2.2f * pulse}, .color = Colors::white.with_alpha(160), .texture = ring, .layer = 5});
            r.draw_text(app.ui_font_bold(), info(c.nucleus).symbol, center - glm::vec2{20.0f, 11.0f},
                        {.size = 18, .color = Colors::white, .align = TextAlign::Center, .max_width = 40, .layer = 6, .shadow = Colors::black});
        });

        // Частицы маны.
        for (const Mote* m : metabolism.motes) {
            r.draw(SpriteInstance{.position = glm::mix(m->previous, m->position, alpha), .size = {5.0f, 5.0f}, .color = Color{200, 255, 230, 200},
                                  .texture = circle, .layer = 2});
        }

        // Связи: одинарная — одна линия, двойная — две, тройная — три; яркость — поток энергии.
        world.view<const Bond>().each([&](const Bond& b) {
            const Body* a = world.get<Body>(b.a);
            const Body* c = world.get<Body>(b.b);
            const Rune* ra = world.get<Rune>(b.a);
            if (a == nullptr || c == nullptr || ra == nullptr) return;
            const glm::vec2 pa = glm::mix(a->previous, a->position, alpha), pc = glm::mix(c->previous, c->position, alpha);
            const glm::vec2 dir = glm::normalize(pc - pa + glm::vec2{1e-4f, 0.0f});
            const glm::vec2 side{-dir.y, dir.x};
            const Color base = ra->aromatic ? Color::from_rgba(0xFFD86BFF) : Color{230, 230, 230, 255};
            const Color col = Color::lerp(base.scaled(0.7f), Colors::white, std::min(b.flux * 3.0f, 1.0f));
            for (int k = 0; k < b.order; ++k) {
                const float offset = (static_cast<float>(k) - (static_cast<float>(b.order) - 1.0f) * 0.5f) * 5.0f;
                r.draw_line(pa + side * offset, pc + side * offset, 3.0f, col, 7);
            }
        });

        // Руны.
        hovered_rune = {};
        const FontHandle bold = app.ui_font_bold();
        world.view<const Rune, const Body>().each([&](ECS::Entity e, const Rune& rune, const Body& b) {
            const glm::vec2 p = glm::mix(b.previous, b.position, alpha);
            if (glm::length(p - input.pointer) < rune_radius) hovered_rune = e;
            const Color col = color_of(rune.element);
            const float flicker = rune.stable ? 1.0f : 0.6f + 0.4f * std::sin(static_cast<float>(app.tick() + e.index * 13) * 0.3f);
            const float glow = std::min(rune.energy / 40.0f, 1.0f);
            if (rune.stable) r.draw(SpriteInstance{.position = p, .size = glm::vec2{rune_radius * 2.0f + 10.0f * glow}, .color = col.with_alpha(80), .texture = circle, .layer = 8});
            r.draw(SpriteInstance{.position = p, .size = glm::vec2{rune_radius * 2.0f}, .color = col.scaled(flicker), .texture = circle, .layer = 9});
            r.draw(SpriteInstance{.position = p, .size = glm::vec2{rune_radius * 2.0f}, .color = rune.aromatic ? Color::from_rgba(0xFFD86BFF) : Color{30, 30, 30, 220},
                                  .texture = ring, .layer = 10});
            r.draw_text(bold, info(rune.element).symbol, p - glm::vec2{14.0f, 8.0f},
                        {.size = 13, .color = Color{20, 20, 20, 255}, .align = TextAlign::Center, .max_width = 28, .layer = 11});
            for (int k = 0; k < rune.free_valence(); ++k) { // свободная валентность — точки вокруг
                const float a = static_cast<float>(k) * 2.0f * pi / static_cast<float>(info(rune.element).valence) + static_cast<float>(app.tick()) * 0.05f;
                r.draw(SpriteInstance{.position = p + glm::vec2{std::cos(a), std::sin(a)} * (rune_radius + 4.0f), .size = {5.0f, 5.0f},
                                      .color = Colors::white, .texture = circle, .layer = 11});
            }
        });

        // Голем.
        const glm::vec2 g = golem_center + glm::vec2{golem.shake > 0 ? std::sin(static_cast<float>(golem.shake)) * 6.0f : 0.0f, 0.0f};
        r.draw(SpriteInstance{.position = g, .size = glm::vec2{golem_radius * 2.0f}, .color = Color::from_rgba(0x6E6A64FF), .texture = circle, .layer = 1});
        r.draw(SpriteInstance{.position = g + glm::vec2{-22, -12}, .size = {16, 16}, .color = Color::from_rgba(0xFF8A3CFF), .texture = circle, .layer = 2});
        r.draw(SpriteInstance{.position = g + glm::vec2{22, -12}, .size = {16, 16}, .color = Color::from_rgba(0xFF8A3CFF), .texture = circle, .layer = 2});
        r.fill_rect({{golem_center.x - 80, golem_center.y + 85}, {160, 12}}, Color{40, 20, 20, 255}, 1);
        r.fill_rect({{golem_center.x - 80, golem_center.y + 85}, {160 * golem.health / 500.0f, 12}}, Color::from_rgba(0xD63B3BFF), 2);
        r.draw_text(app.ui_font(), std::format("Голем {:.0f}/500", golem.health), {golem_center.x - 80, golem_center.y + 100},
                    {.size = 18, .color = Colors::white, .align = TextAlign::Center, .max_width = 160, .layer = 3, .shadow = Colors::black});
        for (const Golem::Number& n : golem.numbers) {
            const float rise = static_cast<float>(80 - n.life);
            r.draw_text(bold, std::format("{}{:.0f}", n.heal ? "+" : "−", n.amount), {golem_center.x - 60 + (n.heal ? 80.0f : 0.0f), golem_center.y - 100 - rise},
                        {.size = 30, .color = (n.heal ? Color::from_rgba(0x66FF88FF) : Color::from_rgba(0xFF5544FF)).with_alpha(static_cast<std::uint8_t>(std::min(255, n.life * 4))),
                         .layer = 30, .shadow = Colors::black});
        }

        // Тянем связь ПКМ.
        if (input.drag_from) {
            if (const Body* b = world.get<Body>(input.drag_from)) r.draw_line(b->position, input.pointer, 2.0f, Color{255, 255, 255, 140}, 12);
        }
    }

    void render_overlay(Core::App& app, Renderer2D& r) override {
        const glm::vec2 vp = app.camera().viewport;
        const FontHandle font = app.ui_font();
        const FontHandle bold = app.ui_font_bold();
        const float x0 = vp.x - 400.0f;
        float y = vp.y * 0.47f;
        r.fill_rect({{x0 - 12, y - 12}, {396, vp.y - y - 20}}, Color{0, 0, 0, 140}, 0);

        // Палитра рун.
        for (std::size_t i = 0; i < elements.size(); ++i) {
            const auto e = static_cast<Element>(i);
            const bool active = input.selected == e;
            r.draw(SpriteInstance{.position = {x0 + 14, y + 12}, .size = {24, 24}, .color = color_of(e), .texture = circle, .layer = 2});
            r.draw_text(active ? bold : font, std::format("{} {} {} · валентность {} · {}", i + 1, info(e).symbol, info(e).name, info(e).valence, info(e).effect),
                        {x0 + 34, y + 2}, {.size = 17, .color = active ? Color::from_rgba(0xFFE36EFF) : Color{220, 220, 220, 255}, .layer = 2});
            y += 26;
        }
        y += 8;
        // Анализ клетки под курсором (или первой).
        ECS::Entity cell = hovered_cell;
        if (!cell) world.view<const Cell>().each([&](ECS::Entity e, const Cell& c) { if (!c.casting) cell = e; });
        if (cell) {
            const Cell& c = *world.get<Cell>(cell);
            const SpellStats s = analyze(world, cell);
            r.draw_text(bold, std::format("Клетка {} · ядро: {}", c.id, info(c.nucleus).name), {x0, y}, {.size = 20, .color = color_of(c.nucleus), .layer = 2});
            y += 26;
            r.draw_text(font, std::format("Формула: {}", s.formula), {x0, y}, {.size = 18, .color = Colors::white, .max_width = 380, .layer = 2});
            y += 24.0f * std::max(1.0f, std::ceil(r.measure_text(font, std::format("Формула: {}", s.formula), {.size = 18, .max_width = 380}).y / 22.0f));
            r.draw_text(font, std::format("Урон {:.0f} · лечение {:.0f} · скорость {:.1f}", s.damage, s.heal, s.speed), {x0, y}, {.size = 17, .color = Color{230, 200, 190, 255}, .layer = 2});
            y += 22;
            r.draw_text(font, std::format("Сила ×{:.2f} · колец {} · радикалов {} · энергия {:.0f}", s.power, s.rings, s.radicals, c.energy), {x0, y},
                        {.size = 17, .color = Color{200, 220, 230, 255}, .layer = 2});
            y += 30;
        }
        for (const std::string& line : chronicle.lines) {
            r.draw_text(font, line, {x0, y}, {.size = 15, .color = Color{190, 190, 180, 255}, .layer = 2});
            y += 19;
        }

        r.draw_text(bold, "RuneCell — заклинание как живая клетка", {16, 12}, {.size = 26, .color = Color::from_rgba(0xFFE36EFF), .layer = 20, .shadow = Colors::black});
        r.draw_text(font, std::format("клеток {} · рун {} · связей {} · маны {} · делений {} · реакций {} · физика {:.2f} мс, обмен {:.2f} мс · {}",
                                      world.count<Cell>(), world.count<Rune>(), world.count<Bond>(), metabolism.motes.size(), chemistry.divisions_done,
                                      chemistry.reactions, physics.last_ms, metabolism.last_ms, to_string(app.device().backend())),
                    {16, 44}, {.size = 16, .color = Color{210, 210, 210, 255}, .layer = 20, .shadow = Colors::black});
        if (input.autoplay) r.draw_text(font, "Автоигра (A — выключить)", {16, 66}, {.size = 16, .color = Color::from_rgba(0x7FD8FFFF), .layer = 20});
        r.draw_text(font, "1–5 руна · ЛКМ — посадить · ПКМ руна→руна — связь, щелчок — ослабить · X — удалить · Space — запустить · N — клетка · A — автоигра",
                    {0, vp.y - 26}, {.size = 15, .color = Color{170, 170, 180, 255}, .align = TextAlign::Center, .max_width = vp.x, .layer = 20});
    }

    [[nodiscard]] std::string status() const override {
        return std::format("cells {} | runes {} | bonds {} | golem {:.0f}", world.count<Cell>(), world.count<Rune>(), world.count<Bond>(), golem.health);
    }

    void shutdown(Core::App& app) override {
        std::size_t stable = 0, aromatic = 0;
        world.view<const Rune>().each([&](const Rune& r) {
            stable += r.stable ? 1 : 0;
            aromatic += r.aromatic ? 1 : 0;
        });
        std::println("\n===== RuneCell : summary =====");
        std::println("cells {} | runes {} ({} stable, {} in rings) | bonds {} | divisions {} | reactions {} | bonds made {} broken {}",
                     world.count<Cell>(), world.count<Rune>(), stable, aromatic, world.count<Bond>(), chemistry.divisions_done, chemistry.reactions,
                     chemistry.bonds_made, chemistry.bonds_broken);
        std::println("mana: {} spawned, {} absorbed, {} live (pool {}) | casts {} | golem damage {:.0f}, heal {:.0f}", metabolism.spawned, metabolism.eaten,
                     metabolism.motes.size(), metabolism.pool.live(), caster.casts, golem.total_damage, golem.healed);
        std::uint64_t checksum = 1469598103934665603ULL;
        const auto mix = [&](std::uint64_t v) { checksum = (checksum ^ v) * 1099511628211ULL; };
        std::vector<std::pair<std::uint32_t, glm::vec2>> runes;
        world.view<const Rune, const Body>().each([&](ECS::Entity e, const Rune&, const Body& b) { runes.emplace_back(e.index, b.position); });
        std::ranges::sort(runes, {}, [](const auto& p) { return p.first; });
        for (const auto& [index, p] : runes) mix(index), mix(std::bit_cast<std::uint32_t>(p.x)), mix(std::bit_cast<std::uint32_t>(p.y));
        mix(metabolism.eaten), mix(chemistry.bonds_made), mix(std::bit_cast<std::uint32_t>(golem.total_damage));
        std::println("jobs: {} background threads | world checksum {:016x}", app.jobs().threads(), checksum);
        metabolism.clear();
    }

private:
    void seed_cells() {
        // Клетка Огня: Ig=Ig (как O₂) и Ve–Ig–Ve (как H₂O) — устойчивые; пара радикалов.
        const ECS::Entity fire = chemistry.add_cell(world, dish_center + glm::vec2{-150.0f, -60.0f}, Element::Ignis);
        const glm::vec2 fc = world.get<Cell>(fire)->center;
        const ECS::Entity a = chemistry.add_rune(world, fire, Element::Ignis, fc + glm::vec2{-30, -25});
        const ECS::Entity b = chemistry.add_rune(world, fire, Element::Ignis, fc + glm::vec2{0, -40});
        chemistry.change_bond(world, a, b, +1, false), chemistry.change_bond(world, a, b, +1, false);
        const ECS::Entity w = chemistry.add_rune(world, fire, Element::Ignis, fc + glm::vec2{30, 25});
        chemistry.change_bond(world, w, chemistry.add_rune(world, fire, Element::Ventus, fc + glm::vec2{55, 20}), +1, false);
        chemistry.change_bond(world, w, chemistry.add_rune(world, fire, Element::Ventus, fc + glm::vec2{20, 50}), +1, false);
        chemistry.add_rune(world, fire, Element::Lux, fc + glm::vec2{-35, 30});

        // Клетка Земли: «бензол» — кольцо из шести Te через одну двойную связь, у каждой — Ve.
        const ECS::Entity earth = chemistry.add_cell(world, dish_center + glm::vec2{160.0f, 80.0f}, Element::Terra);
        const glm::vec2 ec = world.get<Cell>(earth)->center;
        std::array<ECS::Entity, 6> ring_runes{};
        for (int i = 0; i < 6; ++i) {
            const float angle = static_cast<float>(i) * pi / 3.0f;
            ring_runes[static_cast<std::size_t>(i)] = chemistry.add_rune(world, earth, Element::Terra, ec + glm::vec2{std::cos(angle), std::sin(angle)} * 38.0f);
        }
        for (int i = 0; i < 6; ++i) {
            const ECS::Entity x = ring_runes[static_cast<std::size_t>(i)], y = ring_runes[static_cast<std::size_t>((i + 1) % 6)];
            chemistry.change_bond(world, x, y, +1, false);
            if (i % 2 == 0) chemistry.change_bond(world, x, y, +1, false);
            const float angle = static_cast<float>(i) * pi / 3.0f;
            chemistry.change_bond(world, x, chemistry.add_rune(world, earth, Element::Ventus, ec + glm::vec2{std::cos(angle), std::sin(angle)} * 66.0f), +1, false);
        }
        chemistry.add_rune(world, earth, Element::Aqua, ec + glm::vec2{0, 5});

        // Клетка Воды — пустая: её заполняет игрок.
        chemistry.add_cell(world, dish_center + glm::vec2{-60.0f, 220.0f}, Element::Aqua);
        chemistry.classify(world);
    }

    void add_free_cell() {
        if (world.count<Cell>() >= max_cells) return;
        std::uint64_t rng = chemistry.rng ^ world.count<Cell>();
        for (int attempt = 0; attempt < 30; ++attempt) {
            const float a = random01(rng) * 2.0f * pi, d = random01(rng) * (dish_radius - 120.0f);
            const glm::vec2 at = dish_center + glm::vec2{std::cos(a), std::sin(a)} * d;
            bool free = true;
            world.view<const Cell>().each([&](const Cell& c) { free = free && glm::length(c.center - at) > c.radius() + 90.0f; });
            if (free) {
                chemistry.add_cell(world, at, static_cast<Element>(splitmix(rng) % 5));
                return;
            }
        }
    }

    ECS::World world;
    PlayerInput input;
    Autoplay autoplay;
    Chemistry chemistry;
    Physics physics;
    Metabolism metabolism;
    Caster caster;
    Golem golem;
    Chronicle chronicle;
    TextureHandle circle{}, ring{}, dish{};
    ECS::Entity hovered_cell{}, hovered_rune{};
};

} // namespace

int main(int argc, char** argv) {
    return Core::run<RuneCell>({.title = "RuneCell", .width = 1600, .height = 900, .ticks_per_second = 60.0, .pause_key = InputSystem::Key::P,
                                .camera_controls = false, .clear_rgba = 0x101418FF},
                               argc, argv);
}
