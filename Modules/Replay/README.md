# Replay — запись и повтор прогона

Мир меняется только командами внутри тика, поэтому **сид + команды каждого тика полностью задают прогон**. Повтор без ввода
даёт те же хеши состояния на последнем тике; любой баг симуляции воспроизводится файлом записи.

Подключение: `#include <Replay/Replay.hpp>`, цель `engine::Replay`. Зависит от `Math`.

## Путь игры

```cpp
Replay::CommandRegistry registry;                 // схемы команд: имена и типы полей — попадут в файл
registry.add({1, "move", {{}, {"dx", Fixed}, {}, {"dz", Fixed}}});
auto session = Replay::Session::from_args(args, /*seed*/ 1, &registry);   // --record f | --replay f | --seed N
Replay::FlightRecorder flight(256);               // последние тики; при FLUX_ASSERT сбрасываются в файл
Replay::Driver driver(sim, *session, &flight);    // sim: tick(span<Command>), tick_number(), hashes()

// каждый тик:
if (!driver.step(live_commands)) quit();          // при повторе live игнорируется; false — запись кончилась
// в конце:
auto verdict = driver.finish();                   // запись → файл; повтор → сверка хешей
std::puts(Replay::describe(*verdict, session->mode(), ticks, commands).c_str());
```

| Тип | Роль |
|---|---|
| `Command` | 16 байт: `type`, `arg`, `x`, `y`, `z`; смысл полей — в схеме игры |
| `CommandRegistry`, `CommandSchema` | имена команд и полей; `format(command)` → «move dx=1 dz=0» |
| `Recording` | сид, число тиков, команды, хеши на последнем тике, схемы; файл самоописываем (версия 2; версия 1 читается) |
| `Session` | режим `Off` / `Record` / `Replay`, `begin_tick`, `finish` |
| `Driver` | одна строка на тик вместо ручной склейки сессии, журнала и симуляции |
| `FlightRecorder` | кольцо последних тиков: номер, команды, хеши; дамп при `FLUX_ASSERT` |
| `inspect`, `diff` | просмотр записи и первое расхождение двух записей — без знания игры |

Инструмент `ReplayTool inspect|diff` (Sandbox) использует именно `inspect` и `diff`.

Пример — `examples/01_record_and_replay.cpp` (запись, повтор, просмотр, сравнение). Тесты: `ReplayTests`.
