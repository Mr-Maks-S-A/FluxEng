/**
 * @example 01_schedule.cpp
 * Расписание фаз: порядок шагов симуляции — список с именами, а не вызовы в одной функции.
 *
 * Новый модуль (машины, погода) подключается фазой в нужное место, не трогая чужой код.
 * Порядок определяет результат (проверяется ниже), поэтому он должен быть виден.
 */

#include <Phases/Schedule.hpp>

#include <cstdio>
#include <string>

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    // Состояние «игры»: счётчик, который разные фазы меняют по-разному.
    int value = 1;
    std::string trace;

    Phases::Schedule schedule;
    schedule.add("input", [&] { trace += "input "; value += 2; })
        .add("physics", [&] { trace += "physics "; value *= 3; })
        .add("events", [&] { trace += "events "; });

    schedule.run();
    std::printf("1. порядок: %s→ value = %d  ((1+2)·3)\n", trace.c_str(), value);
    EXPECT(value == 9);

    // 2. Новый модуль подключает фазу после физики: «машины» видят результат физики и не требуют правки чужого кода.
    trace.clear();
    const bool ok = schedule.insert_after("physics", "machines", [&] { trace += "machines "; value += 100; });
    EXPECT(ok);
    EXPECT(!schedule.insert_after("нет такой", "ghost", [] {})); // нет якоря — ничего не добавлено
    value = 1;
    schedule.run();
    std::printf("2. после вставки: %s→ value = %d\n", trace.c_str(), value);
    EXPECT(value == 109);

    // 3. Фазу можно выключить для отладки («а что, если физики нет?») и убрать совсем.
    schedule.set_enabled("physics", false);
    value = 1;
    schedule.run();
    std::printf("3. без physics: value = %d\n", value);
    EXPECT(value == 103);
    schedule.set_enabled("physics", true);
    EXPECT(schedule.remove("machines"));

    // 4. Замеры по фазам собираются сами — для оверлея и профилирования.
    schedule.run();
    std::printf("4. фазы последнего тика:");
    for (const Phases::PhaseTime& t : schedule.times()) std::printf(" %.*s=%.3fms", static_cast<int>(t.name.size()), t.name.data(), t.milliseconds);
    std::printf(" | всего %.3f ms\n", schedule.total_ms());
    EXPECT(schedule.times().size() == 3);
    std::printf("OK\n");
    return 0;
}
