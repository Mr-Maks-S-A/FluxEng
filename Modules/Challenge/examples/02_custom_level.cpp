/**
 * @example 02_custom_level.cpp
 * Свой уровень в коде: сид, правка поверх сида (холм-препятствие), цель, пределы и планки звёзд. Две попытки одного
 * уровня — «в лоб» и экономная — дают разные звёзды: ради этой разницы игрок и перерисовывает заклинание.
 *
 * Уровень — только данные: правила симуляции он не трогает. Заклинания игрок передаёт командой `SetProgram`
 * (блоб с программой + хеш в команде), как это делает редактор рун в игре.
 */

#include <Challenge/Play.hpp>

#include <cstdio>

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    using Math::Fixed;
    using Math::WorldPos;

    // Уровень «Колодец»: вскрыть пустоту в точке на 1,5 м под землёй рядом с магом.
    Challenge::Level level;
    level.id = "well";
    level.title = "Колодец";
    level.brief = "Вскройте точку на 1,5 м под ногами. Чем дешевле заклинание, тем больше звёзд.";
    level.config.seed = 7;
    level.config.grimoire = {"", "", ""};                 // слоты пусты: заклинание игрок приносит сам
    const WorldPos feet_estimate = WorldPos::from_meters(64, 0, 64);
    SpellSim::Simulation probe(level.config);              // рельеф сида: ставим цель относительно земли
    const std::int64_t ground = probe.terrain().ground_height(feet_estimate.x, feet_estimate.z);
    level.goal = {Challenge::GoalKind::Carve, {feet_estimate.x, ground - Fixed::from_ratio(3, 2).raw, feet_estimate.z}, {}};
    level.limits = {.max_ticks = 60 * 10, .max_casts = 2};
    level.par = {.mana_spent = 170, .program_runes = 4}; // ★★ — дешевле 170 маны, ★★★ — и не длиннее 4 рун

    const Math::FVec3 straight_down{Fixed{}, Fixed::from_int(-1), Fixed{}};
    const auto attempt = [&](const char* spell) {
        Challenge::Solution s;
        s.programs = {{0, spell}};                                   // уйдёт блобом + командой SetProgram на тике 0
        s.steps = {{30, SpellSim::cast_command(0, Runes::ManaSource::Personal, straight_down)}};
        return Challenge::play(level, s);
    };

    // Попытка 1: «в лоб» — широкий шар, дорого (r=3: 810 маны).
    const Challenge::Outcome brute = attempt("TARGET\nPUSH 3\nCARVE\nHALT\n");
    std::printf("в лоб:    %s\n", brute.status_line.c_str());
    // Попытка 2: цель на 1,5 м под центром прицела, значит нужен радиус чуть больше 1,5 (r=1,75: 160 маны).
    const Challenge::Outcome thrifty = attempt("TARGET\nPUSH 1.75\nCARVE\nHALT\n");
    std::printf("экономно: %s\n", thrifty.status_line.c_str());

    EXPECT(brute.status == Challenge::Status::Won);
    EXPECT(thrifty.status == Challenge::Status::Won);
    EXPECT(thrifty.metrics.mana_spent < brute.metrics.mana_spent);
    EXPECT(brute.stars < thrifty.stars);
    EXPECT(thrifty.stars == 3);
    std::printf("OK\n");
    return 0;
}
