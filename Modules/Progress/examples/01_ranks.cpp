/**
 * @example 01_ranks.cpp
 * Путь чародея: заклинание с переменными закрыто для новичка и открывается опытом.
 *
 * Чародей делает эффекты (каждый приносит опыт по радиусу), ранг растёт, а с ним открываются руны языка.
 */

#include <Progress/Progress.hpp>

#include <cstdio>

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    const Progress::RankTable ranks = Progress::RankTable::default_table();
    const auto counter = Runes::parse_program(
        "PUSH 3\nSTORE 0\nloop:\nTARGET\nPUSH 1\nCARVE\nLOAD 0\nPUSH 1\nSUB\nSTORE 0\nPUSH 0\nLOAD 0\nLT\nJMP_IF loop\nHALT\n", "counter");
    EXPECT(counter.has_value());

    Progress::Mastery me;
    const auto locked = ranks.first_locked(*counter, me.rank);
    EXPECT(locked.has_value());
    std::printf("Ранг «%s»: заклинание закрыто — руна %s откроется на ранге %d\n", ranks.at(me.rank).title.c_str(), std::string(Runes::rune_name(*locked)).c_str(),
                ranks.rank_of_rune(*locked));

    int effects = 0;
    while (ranks.first_locked(*counter, me.rank)) {
        const Progress::Award a = Progress::award(ranks, me, Progress::effect_xp(Math::Fixed::from_int(3))); // шар радиуса 3 м
        ++effects;
        if (a.ranked_up()) std::printf("  после %d эффектов: ранг «%s» (опыт %u)\n", effects, ranks.at(me.rank).title.c_str(), me.xp);
    }
    std::printf("Заклинание открыто на ранге «%s»: нужно было %d эффектов, максимум маны +%.0f\n", ranks.at(me.rank).title.c_str(), effects, ranks.mana_bonus(me.rank).to_double());
    EXPECT(me.rank == 2);
    EXPECT(effects == 30); // 300 опыта по 10 за шар радиуса 3
    return 0;
}
