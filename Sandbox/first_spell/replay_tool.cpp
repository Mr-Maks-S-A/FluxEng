/**
 * @file replay_tool.cpp
 * @brief ReplayTool — просмотр и сравнение записей `--record` без запуска игры и без знания её команд.
 *
 *   ReplayTool inspect run.rec [N]    заголовок, схемы команд, первые N команд (по умолчанию все)
 *   ReplayTool diff a.rec b.rec       первое расхождение двух записей; код выхода 1, если они различаются
 *   ReplayTool repair run.rec         исправить повреждённые блоки файла на месте (запись — журнал с избыточностью)
 *
 * Файл записи самоописываем (таблица схем команд внутри), поэтому инструмент зависит только от модуля Replay.
 */

#include <Replay/Replay.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "inspect" && argc >= 3) {
        auto loaded = Replay::Recording::load_with_info(argv[2]); // показывает и то, что повреждено или потеряно
        if (!loaded) {
            std::fprintf(stderr, "%s\n", loaded.error().c_str());
            return 2;
        }
        const std::size_t limit = argc >= 4 ? static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10)) : static_cast<std::size_t>(-1);
        std::fputs(Replay::describe(loaded->second).c_str(), stdout);
        std::fputs(Replay::inspect(loaded->first, limit).c_str(), stdout);
        return 0;
    }
    if (mode == "diff" && argc >= 4) {
        auto a = Replay::Recording::load(argv[2]);
        auto b = Replay::Recording::load(argv[3]);
        if (!a || !b) {
            std::fprintf(stderr, "%s\n", (!a ? a.error() : b.error()).c_str());
            return 2;
        }
        if (const auto difference = Replay::diff(*a, *b)) {
            std::printf("различаются: %s\n", difference->text.c_str());
            return 1;
        }
        std::puts("записи совпадают: сид, команды, длина и хеши");
        return 0;
    }
    if (mode == "repair" && argc >= 3) {
        const auto report = Replay::Recording::repair_file(argv[2]);
        if (!report) {
            std::fprintf(stderr, "%s\n", report.error().c_str());
            return 2;
        }
        std::printf("исправлено блоков: %llu, не восстановлено: %llu, потеряно записей: %llu\n", static_cast<unsigned long long>(report->blocks_repaired),
                    static_cast<unsigned long long>(report->blocks_lost), static_cast<unsigned long long>(report->records_lost));
        return report->records_lost == 0 ? 0 : 1;
    }
    std::fputs("использование:\n  ReplayTool inspect файл.rec [N]\n  ReplayTool diff a.rec b.rec\n  ReplayTool repair файл.rec\n", stderr);
    return 2;
}
