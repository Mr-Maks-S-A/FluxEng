# Phases — расписание фаз тика

Порядок шагов симуляции — часть правил детерминизма, поэтому он должен быть **виден**: список именованных фаз, а не вызовы,
вшитые в один большой `tick()`. Новый модуль (машины, погода, гравитация тел) подключается фазой и не требует правки чужого кода.
Замеры по каждой фазе собираются сами.

Подключение: `#include <Phases/Schedule.hpp>`, цель `engine::Phases`. Зависит от `Math` (только `FLUX_ASSERT`).

```cpp
Phases::Schedule schedule;
schedule.add("input",   [&] { apply_input(); })
        .add("physics", [&] { step_physics(); });
schedule.insert_after("physics", "machines", [&] { step_machines(); });   // новый модуль, чужой код не тронут
schedule.set_enabled("physics", false);                                  // отладка: «а что, если без физики?»
schedule.run();                                                          // по порядку, с замерами
for (auto& t : schedule.times()) log(t.name, t.milliseconds);
```

| Метод | Что делает |
|---|---|
| `add`, `insert_before`, `insert_after`, `remove` | управление порядком; неизвестный якорь — `false`, повторное имя — `FLUX_ASSERT` |
| `set_enabled` | выключить фазу, не убирая из расписания |
| `run` | выполнить включённые фазы по порядку, замерить каждую |
| `times`, `total_ms`, `names` | замеры и порядок для оверлея и отладки |

Пример — `examples/01_schedule.cpp` (порядок определяет результат; вставка, выключение, замеры). Тесты: `PhasesTests`.
Время — только для отладки, в состояние симуляции оно не попадает.
