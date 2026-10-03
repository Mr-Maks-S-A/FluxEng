# Replay — запись и повтор прогона

Мир меняется только командами внутри тика, поэтому **сид + команды каждого тика полностью задают прогон**. Повтор без ввода
даёт те же хеши состояния на последнем тике; любой баг симуляции воспроизводится файлом записи.

Подключение: `#include <Replay/Replay.hpp>`, цель `engine::Replay`. Зависит от `Math` и `EventLog` (формат файла).

## Путь игры

```cpp
Replay::CommandRegistry registry;                 // схемы команд: имена и типы полей — попадут в файл
registry.add({1, "move", {{}, {"dx", Fixed}, {}, {"dz", Fixed}}});
auto session = Replay::Session::from_args(args, /*seed*/ 1, &registry).value();   // --record f | --replay f | --seed N
Replay::FlightRecorder flight(256);               // последние тики; при FLUX_ASSERT сбрасываются в файл
Replay::Driver driver(sim, session, &flight);     // sim: tick(span<Command>), tick_number(), hashes()

// каждый тик:
if (!driver.step(live_commands)) quit();          // при повторе live игнорируется; false — запись кончилась
// в конце:
auto verdict = driver.finish().value();           // запись → трейлер с хешами; повтор → сверка по подсистемам
std::puts(Replay::describe(verdict, session.mode(), ticks, commands).c_str());
```

## Хеши подсистем названы

`StateHashes` — до 8 пар «имя — значение»: `terrain`, `mana`, `characters`, `spells`, `rng`, `tick`. При расхождении повтора отчёт
называет подсистему (`разошлись подсистемы: mana`), `Replay::diff` — тоже. Симуляция сама решает, какие подсистемы различать:

```cpp
Replay::StateHashes hashes() const { Replay::StateHashes h; h.add("terrain", ...).add("mana", ...); return h; }
```

## Файл записи — журнал с избыточностью (EventLog)

- Команды пишутся **по ходу игры** и сбрасываются на диск раз в секунду симуляции: при падении теряется не больше секунды; оборванная
  запись читается (`complete = false`, финальных хешей нет, повтор идёт до последней команды, сверять нечего).
- Испорченные блоки файла восстанавливаются по чётности при чтении; `Recording::repair_file` / `ReplayTool repair` переписывает их на месте.
- Если команды потеряны сверх избыточности, `Recording::load` отказывает («повтор невозможен»): молча разошедшийся повтор хуже ошибки;
  `load_with_info` всё равно отдаёт то, что осталось, и отчёт `LoadInfo::recovery` (для `inspect`).
- Файлы старого формата (версии 1 и 2) читаются; их хеши получают имена `h0`…`h2`.

| Тип | Роль |
|---|---|
| `Command` | 16 байт: `type`, `arg`, `x`, `y`, `z`; смысл полей — в схеме игры |
| `StateHashes`, `NamedHash` | хеши подсистем по именам; `differing`, `describe` |
| `CommandRegistry`, `CommandSchema` | имена команд и полей; `format(command)` → «move dx=1 dz=0» |
| `Recording`, `LoadInfo` | сид, команды, трейлер с хешами, схемы; `load`, `load_with_info`, `save`, `repair_file` |
| `Session` | режим `Off` / `Record` / `Replay`; запись потоково в файл; `begin_tick`, `finish` |
| `Driver` | одна строка на тик вместо ручной склейки сессии, журнала и симуляции |
| `FlightRecorder` | кольцо последних тиков: номер, команды, хеши; дамп при `FLUX_ASSERT` |
| `inspect`, `describe(LoadInfo)`, `diff` | просмотр записи, отчёт о чтении, первое расхождение двух записей |

Инструмент `ReplayTool inspect | diff | repair` (Sandbox) использует именно эти функции.

Пример — `examples/01_record_and_replay.cpp` (запись, повтор, просмотр, сравнение). Тесты: `ReplayTests`.
