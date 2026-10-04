/**
 * @example 01_play_campaign.cpp
 * Кампания целиком: каждый встроенный уровень проходит эталонное решение (программы + бот), судья выдаёт исход и звёзды.
 * Так выглядит весь цикл «цель → ограничения → попытка → оценка» без окна и графики.
 *
 * Это же доказательство проходимости: если уровень нельзя пройти, пример падает.
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
    int total_stars = 0;
    for (const Challenge::Level& level : Challenge::campaign()) {
        std::printf("\n[%s] %s\n  %s\n", level.id.c_str(), level.title.c_str(), level.brief.c_str());

        // «Ничего не делать» — контрольная попытка: ни один уровень не проходится стоя на месте.
        const Challenge::Outcome idle = Challenge::play_idle(level);
        std::printf("  стоя на месте: %s\n", idle.status == Challenge::Status::Won ? "проходит" : "не проходит");
        EXPECT(idle.status == Challenge::Status::Lost);

        const Challenge::Solution* solution = Challenge::reference_solution(level.id);
        EXPECT(solution != nullptr);
        const Challenge::Outcome outcome = Challenge::play(level, *solution);
        std::printf("  эталон: %s\n", outcome.status_line.c_str());
        EXPECT(outcome.status == Challenge::Status::Won);
        total_stars += outcome.stars;
    }
    std::printf("\nзвёзд всего: %d из %zu\n", total_stars, Challenge::campaign().size() * 3);
    EXPECT(total_stars == static_cast<int>(Challenge::campaign().size()) * 3);
    std::printf("OK\n");
    return 0;
}
