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
Replay::StateHashes h = sim.hashes();                              // ландшафт, мана, ECS (персонаж, заклинания, Rng, тик)
```

## Состояние наружу — только для чтения

`terrain()`, `mana()`, `world()`, `spells()` — `const`: изменить мир мимо команд нельзя, поэтому запись всегда воспроизводима.
«Двери»: команды в `tick`, `take_dirty_chunks()` (очередь на перестройку мешей), `reload_spells`, `programs()`, `set_grimoire_slot`.
События шины: `TerrainEditedEvent` (границы правки), `Runes::SpellFailedEvent`, `SpellFinishedEvent`.

Пример — `examples/01_headless_simulation.cpp` (прогон, детерминизм, запись и повтор, своя фаза). Тесты: `SpellSimTests`
(порядок тика, оба заклинания, дыра в тумане 5–10 с, бесконечный цикл, повтор с тем же хешем).
