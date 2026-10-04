#include <Challenge/Level.hpp>

#include <cmath>
#include <limits>
#include <map>
#include <optional>

namespace Challenge {

namespace {

using Math::Fixed;
using Math::FVec3;

constexpr double m = static_cast<double>(Fixed::one_raw);

double meters_of(std::int64_t raw) { return static_cast<double>(raw) / m; }

/// Прицел, при котором луч из глаз мага попадает в рельеф ближе всего к `wanted`: перебор угла места при курсе на точку.
/// Бот целится так же, как игрок: поворачивает прицел, пока точка попадания не станет нужной.
std::optional<FVec3> aim_to(const SpellSim::Simulation& sim, Math::WorldPos wanted, double* error_m = nullptr) {
    const Math::WorldPos eye = Character::eye(sim.world(), sim.player());
    double dx = meters_of(wanted.x - eye.x), dz = meters_of(wanted.z - eye.z);
    const double flat = std::sqrt(dx * dx + dz * dz);
    if (flat < 0.01) return std::nullopt;
    dx /= flat, dz /= flat;
    double best = 1e18;
    std::optional<FVec3> out;
    for (double pitch = -1.2; pitch <= 0.4; pitch += 0.004) {
        const FVec3 dir{Fixed::from_double(dx * std::cos(pitch)), Fixed::from_double(std::sin(pitch)), Fixed::from_double(dz * std::cos(pitch))};
        const auto hit = sim.terrain().raycast(eye, Math::normalize(dir), Fixed::from_int(40));
        if (!hit) continue;
        const double ex = meters_of(hit->position.x - wanted.x), ey = meters_of(hit->position.y - wanted.y), ez = meters_of(hit->position.z - wanted.z);
        const double err = std::sqrt(ex * ex + ey * ey + ez * ez);
        if (err < best) { best = err; out = Math::normalize(dir); }
    }
    if (error_m != nullptr) *error_m = best;
    return out;
}

/// Поверхность рельефа над/под точкой цели: сюда нужно попасть лучом, чтобы шар достал до цели.
Math::WorldPos surface_near(const SpellSim::Simulation& sim, Math::WorldPos goal) {
    return {goal.x, sim.terrain().ground_height(goal.x, goal.z), goal.z};
}

/// Бот «один прицельный каст на тике 40» (ждёт, пока маг приземлится).
Solution::Policy single_cast(Math::WorldPos goal, int slot, Runes::ManaSource source) {
    // Без собственного состояния (решение общее на все прогоны): «уже кастовал» читается из мира.
    return [=](const SpellSim::Simulation& sim, std::vector<Replay::Command>& out) {
        if (sim.casts() + sim.failed_casts() != 0 || sim.tick_number() < 40) return;
        if (const auto aim = aim_to(sim, surface_near(sim, goal))) out.push_back(SpellSim::cast_command(slot, source, *aim));
    };
}

/// Бот «пробиться к цели»: идёт к ней; упёрся в стену — бьёт заклинанием, когда хватает маны.
Solution::Policy breakthrough(Math::WorldPos goal, std::int32_t mana_needed) {
    return [=](const SpellSim::Simulation& sim, std::vector<Replay::Command>& out) {
        const Math::WorldPos feet = sim.world().get<Character::Position>(sim.player())->value;
        double dx = meters_of(goal.x - feet.x), dz = meters_of(goal.z - feet.z);
        const double len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.01) return;
        dx /= len, dz /= len;
        if (sim.tick_number() % 10 == 0) out.push_back(SpellSim::move_command(Fixed::from_double(dx), Fixed::from_double(dz)));
        // Впереди стена? Луч на уровне груди ближе 4,5 м.
        const Math::WorldPos eye = Character::eye(sim.world(), sim.player());
        const FVec3 forward{Fixed::from_double(dx), Fixed::from_double(-0.12), Fixed::from_double(dz)};
        const auto hit = sim.terrain().raycast(eye, Math::normalize(forward), Fixed::from_int(40));
        const Character::ManaPool& pool = *sim.world().get<Character::ManaPool>(sim.player());
        if (hit && meters_of(hit->position.x - eye.x) * dx + meters_of(hit->position.z - eye.z) * dz < 4.5 && pool.current >= Math::Mana::from_int(mana_needed) && sim.tick_number() % 30 == 0) {
            out.push_back(SpellSim::cast_command(0, Runes::ManaSource::Personal, Math::normalize(forward)));
        }
    };
}

std::map<std::string, Solution, std::less<>> build() {
    std::map<std::string, Solution, std::less<>> out;
    const auto level_of = [](std::string_view id) -> const Level& { return *find_level(id); };

    {   // walk: идти к отметке (бот: решение «по тикам» не годится для сетевой игры, бот видит свой мир).
        Solution s;
        s.policy = breakthrough(level_of("walk").goal.point, std::numeric_limits<std::int32_t>::max());
        out["walk"] = std::move(s);
    }
    {   // mine: один прицельный выстрел в поверхность над точкой.
        Solution s;
        s.policy = single_cast(level_of("mine").goal.point, 0, Runes::ManaSource::Personal);
        out["mine"] = std::move(s);
    }
    {   // mound: свой RAISE r=2 и каст из окружения в поверхность под точкой.
        Solution s;
        s.programs = {{0, "TARGET\nPUSH 2\nRAISE\nHALT\n"}};
        s.policy = single_cast(level_of("mound").goal.point, 0, Runes::ManaSource::Ambient);
        out["mound"] = std::move(s);
    }
    {   // fort: идти на восток и бить в стену «dig», когда хватает маны.
        Solution s;
        s.policy = breakthrough(level_of("fort").goal.point, 240);
        out["fort"] = std::move(s);
    }
    return out;
}

} // namespace

const Solution* reference_solution(std::string_view id) {
    static const auto all = build();
    const auto it = all.find(id);
    return it == all.end() ? nullptr : &it->second;
}

} // namespace Challenge
