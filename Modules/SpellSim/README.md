# SpellSim — симуляция «Первого заклинания»

Вся игра без окна и графики: ландшафт, поле маны, ECS с персонажем и заклинаниями, генератор с сидом — один объект
`Simulation`. Тесты, повторы (`--replay`) и сетевая симуляция запускают именно его.

Подключение: `#include <SpellSim/SpellSim.hpp>`, цель `engine::SpellSim`. Зависит от `Math`, `Phases`, `Replay`, `Terrain`,
`ManaField`, `Runes`, `Character`, `EventSystem`, `ECSSystem`.

## Тик (60 Гц) — расписание фаз

`commands → spells → terrain → mana → movement → events` — это `Phases::Schedule`: порядок и замеры видны, новый модуль
подключается `sim.schedule().insert_after("movement", "machines", fn)`. Заклинания меняют мир только данными (`Effect`),
которые фаза `terrain` применяет в порядке исполнения; поле маны шагает каждый 6-й тик (10 Гц).

## Команды

Мир меняется **только командами**: `MoveCommand{dx, dz}`, `JumpCommand`, `CastCommand{slot, source, aim}`. Игра превращает ввод
в команды (float → Fixed один раз, `quantize_direction`), `encode()` / `decode()` кладут их в `Replay::Command`,
`register_commands(registry)` даёт схемы для самоописываемой записи.

```cpp
SpellSim::Simulation sim(SpellSim::Config{.seed = 11});
sim.programs().add_text("carve", "TARGET\nPUSH 2\nCARVE\nHALT\n");
Replay::Driver driver(sim, session, &flight);                      // Simulation — это Replay::Simulatable
driver.step({SpellSim::CastCommand{0, Runes::ManaSource::Personal, aim}.encode()});
Replay::StateHashes h = sim.hashes();                              // по именам: terrain, mana, characters, spells, rng, tick
```

### `SetProgram` — правки редактора в потоке команд

`SetProgramCommand{slot, hash}` ставит в слот гримуара программу **по хешу содержимого**. Сама программа (`Runes::encode_program`) не
влезает в 16 байт и приходит отдельно — блобом: `Simulation::provide_program(bytes)` проверяет её (`decode_program`), кладёт в библиотеку
под именем `#<хеш>` и возвращает хеш (число различных программ ограничено `Config::max_custom_programs`). Дальше команда
`set_program_command(slot, hash)` идёт как любая другая: **записывается, воспроизводится и уходит по сети**. Нет блоба с таким хешем или неверный слот —
команда отклоняется (`rejected_programs()`), слот остаётся прежним: отказ одинаков у всех, симуляция не расходится.

```cpp
const auto hash = driver.submit_blob(Runes::encode_program(program));   // Replay::Driver: блоб в запись и в симуляцию
live.push_back(SpellSim::set_program_command(1, hash));                 // на ближайшем тике слот 1 держит эту программу
```

Для сети: `SpellSim::blob_reference` — «какой блоб нужен команде» для `Net::Lockstep::set_blob_reference`.

### Уровни поверх сида

`Config::setup` — правки ландшафта при создании мира (`SetupEdit`: шар породы или пустоты), `spawn_offset_x_m/z_m` — сдвиг точки
появления. На них стоит модуль `Challenge`. Это часть настройки, не команда: в записи она известна по номеру уровня.

## Состояние наружу — только для чтения

`terrain()`, `mana()`, `world()`, `spells()` — `const`: изменить мир мимо команд нельзя, поэтому запись всегда воспроизводима.
«Двери»: команды в `tick`, `take_dirty_chunks()` (очередь на перестройку мешей), `reload_spells`, `programs()`, `set_grimoire_slot`.
События шины: `TerrainEditedEvent` (границы правки), `Runes::SpellFailedEvent`, `SpellFinishedEvent`.

## Эталонные записи (golden)

`tests/golden/*.rec` — короткие записи сценариев с хешами каждой подсистемы на последнем тике, закоммиченные в репозиторий. Тест `Golden`
воспроизводит их и требует тех же хешей — **в любой сборке** (gcc и clang, Release и Debug дают одно и то же: проверено). Так потеря
детерминизма ловится сразу, с названием разошедшейся подсистемы. Если поведение изменено намеренно: `tools/update_golden.sh`
(`FLUX_UPDATE_GOLDEN=1`), новые файлы коммитятся вместе с изменением.

Пример — `examples/01_headless_simulation.cpp` (прогон, детерминизм, запись и повтор, своя фаза). Тесты: `SpellSimTests`
(порядок тика, оба заклинания, дыра в тумане 5–10 с, бесконечный цикл, повтор с тем же хешем).
