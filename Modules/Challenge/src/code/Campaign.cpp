#include <Challenge/Level.hpp>

#include <Math/Hash.hpp>
#include <Terrain/Terrain.hpp>

#include <cmath>
#include <deque>

namespace Challenge {

namespace {

using Math::Fixed;
using Math::WorldPos;

constexpr std::int64_t m_raw = Fixed::one_raw;
constexpr const char* dig3_text = "# вырезать шар r=3 в точке прицела (810 маны)\nTARGET\nPUSH 3\nCARVE\nHALT\n";
constexpr const char* dig2_text = "# вырезать шар r=2 в точке прицела (240 маны)\nTARGET\nPUSH 2\nCARVE\nHALT\n";

WorldPos meters(double x, double y, double z) { return WorldPos::from_doubles(x, y, z); }

/// Строит уровни по рельефу сида: стены и цели ставятся относительно земли, а не по «магическим» числам.
class Builder {
public:
    explicit Builder(std::uint64_t seed) : m_world(seed), m_seed(seed) {}

    [[nodiscard]] double ground(double x, double z) const { return static_cast<double>(m_world.ground_height(static_cast<std::int64_t>(x * m_raw), static_cast<std::int64_t>(z * m_raw))) / m_raw; }
    [[nodiscard]] double center_x() const { return static_cast<double>(m_world.layout().size_x()) / 2.0 / m_raw; }
    [[nodiscard]] double center_z() const { return static_cast<double>(m_world.layout().size_z()) / 2.0 / m_raw; }

    /// Ближайшая к магу в (cx, cz) точка рельефа (от `min_r` метров), которая видна из его глаз: рельеф между ними не заслоняет.
    /// Так цель «шахты» и «кургана» достижима прицельным лучом, а не спрятана за бугром.
    [[nodiscard]] std::pair<double, double> visible_point(double cx, double cz, double min_r) const {
        const double eye_y = ground(cx, cz) + 1.7;
        for (double r = min_r; r < 20.0; r += 0.5) {
            for (int k = 0; k < 24; ++k) {
                const double a = 2.0 * 3.14159265358979 * k / 24.0;
                const double px = cx + r * std::cos(a), pz = cz + r * std::sin(a);
                const double gy = ground(px, pz);
                bool clear = true;
                for (double t = 0.5; t < r && clear; t += 0.5) {
                    const double ray_y = eye_y + (gy - eye_y) * t / r;
                    clear = ground(cx + t * std::cos(a), cz + t * std::sin(a)) < ray_y - 0.2;
                }
                if (clear) return {px, pz};
            }
        }
        return {cx + min_r, cz};
    }

    /// Кольцо стены вокруг точки: шары в три слоя по высоте вдоль земли (понизу, посередине, поверху) — не перелезть и не подкопаться.
    void ring(SpellSim::Config& config, double cx, double cz, double radius, double ball) const {
        const int count = static_cast<int>(std::ceil(2.0 * 3.14159265358979 * radius / (ball * 0.9)));
        for (int i = 0; i < count; ++i) {
            const double a = 2.0 * 3.14159265358979 * i / count;
            const double x = cx + radius * std::cos(a), z = cz + radius * std::sin(a);
            const double g = ground(x, z);
            for (const double dy : {-2.0, 0.5, 3.0, 5.5}) {
                config.setup.push_back({.carve = false, .center = meters(x, g + dy, z), .radius = Fixed::from_double(ball)});
            }
        }
    }

private:
    Terrain::SdfWorld m_world;
    std::uint64_t m_seed;
};

SpellSim::Config base_config(std::uint64_t seed) {
    SpellSim::Config config;
    config.seed = seed;
    config.grimoire = {"", "", ""}; // слоты пусты: заклинание уровень выдаёт сам (library) или игрок рисует
    return config;
}

std::deque<Level> build_levels() {
    std::deque<Level> levels;
    Builder b(1);
    const double cx = b.center_x(), cz = b.center_z();

    // 1. Первый шаг: дойти. Учит движению; заклинания не нужны.
    {
        Level l;
        l.id = "walk";
        l.title = "Первый шаг";
        l.brief = "Дойдите до отметки (WASD). Заклинания пока не нужны.";
        l.config = base_config(1);
        l.goal = {GoalKind::Reach, meters(cx + 12, b.ground(cx + 12, cz), cz), Fixed::from_int(2)};
        l.limits = {.max_ticks = 60 * 20};
        l.needs_spell = false;
        levels.push_back(std::move(l));
    }
    // 2. Шахта: пробить породу до точки под землёй. Учит прицеливанию и цене: шар r=3 стоит 810 из 1000.
    {
        Level l;
        l.id = "mine";
        l.title = "Шахта";
        l.brief = "Вскройте подземную камеру: в отмеченной точке под землёй должна появиться пустота. Заклинание «dig» уже в слоте 1.";
        l.config = base_config(1);
        l.config.grimoire = {"dig", "", ""};
        l.library = {{"dig", dig3_text}};
        const auto [x, z] = b.visible_point(cx, cz, 8.0);
        l.goal = {GoalKind::Carve, meters(x, b.ground(x, z) - 2.0, z), {}};
        l.limits = {.max_ticks = 60 * 40, .max_casts = 3};
        l.par = {.mana_spent = 820, .program_runes = 4};
        levels.push_back(std::move(l));
    }
    // 3. Курган: насыпать породу в воздухе. Шар r=2 стоит 240 маны, а предел трат — 150: платить придётся из окружения (Ambient, с мага берётся лишь 1/10, но поле вокруг конечно — ~320).
    {
        Level l;
        l.id = "mound";
        l.title = "Курган";
        l.brief = "Насыпьте породу в отмеченной точке над землёй. Платить из личного запаса слишком дорого (предел трат 150) — берите ману из окружения (ПКМ): с мага уходит лишь десятая часть. Заклинание нарисуйте сами в редакторе.";
        l.config = base_config(1);
        const auto [x, z] = b.visible_point(cx, cz, 8.0);
        l.goal = {GoalKind::Raise, meters(x, b.ground(x, z) + 1.5, z), {}};
        l.limits = {.max_ticks = 60 * 40, .max_casts = 4, .max_mana_spent = 150};
        l.par = {.mana_spent = 60, .program_runes = 4};
        levels.push_back(std::move(l));
    }
    // 4. Крепость: дойти до центра кольцевой стены. Подкоп или перелезть нельзя — прорубать по шагам, экономя ману.
    {
        Level l;
        l.id = "fort";
        l.title = "Крепость";
        l.brief = "Центр окружён стеной толщиной ~5 м. Прорубитесь внутрь и дойдите до отметки: «dig» вырезает шар r=2 (240 маны из 1000, запас восстанавливается 20/с) — касты надо экономить.";
        l.config = base_config(1);
        l.config.grimoire = {"dig", "", ""};
        l.library = {{"dig", dig2_text}};
        const double fx = cx + 16;
        b.ring(l.config, fx, cz, 7.0, 2.5);
        l.goal = {GoalKind::Reach, meters(fx, b.ground(fx, cz), cz), Fixed::from_int(2)};
        l.limits = {.max_ticks = 60 * 60, .max_casts = 8};
        l.par = {.mana_spent = 1300, .program_runes = 4};
        levels.push_back(std::move(l));
    }
    return levels;
}

const std::deque<Level>& levels() {
    static const std::deque<Level> all = build_levels();
    return all;
}

} // namespace

std::span<const Level> campaign() {
    static const std::vector<Level> flat(levels().begin(), levels().end());
    return flat;
}

const Level* find_level(std::string_view id) {
    for (const Level& l : campaign()) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

void install_library(const Level& level, SpellSim::Simulation& sim) {
    for (const auto& [name, text] : level.library) (void)sim.programs().add_text(name, text);
}

std::uint64_t level_hash(const Level& level) {
    Math::Hasher h;
    const auto add_string = [&h](const std::string& s) {
        h.add(s.size());
        for (const char c : s) h.add_byte(static_cast<std::uint8_t>(c));
    };
    add_string(level.id);
    const SpellSim::Config& c = level.config;
    h.add(c.seed);
    h.add_signed(c.spawn_offset_x_m);
    h.add_signed(c.spawn_offset_z_m);
    for (const SpellSim::SetupEdit& e : c.setup) {
        h.add(e.carve ? 1 : 0);
        h.add_signed(e.center.x), h.add_signed(e.center.y), h.add_signed(e.center.z);
        h.add_signed(e.radius.raw);
    }
    for (const std::string& g : c.grimoire) add_string(g);
    for (const auto& [name, text] : level.library) { add_string(name); add_string(text); }
    h.add(static_cast<std::uint64_t>(level.goal.kind));
    h.add_signed(level.goal.point.x), h.add_signed(level.goal.point.y), h.add_signed(level.goal.point.z);
    h.add_signed(level.goal.radius.raw);
    h.add(level.limits.max_ticks), h.add(level.limits.max_casts), h.add(level.limits.max_program_runes), h.add_signed(level.limits.max_mana_spent);
    return h.value();
}

} // namespace Challenge
