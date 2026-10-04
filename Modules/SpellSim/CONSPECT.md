# SpellSim — конспект для изучения и переписывания

`src/include/SpellSim/SpellSim.hpp` (229 строк) и `src/code/SpellSim.cpp` (~220 строк). Это **узел**, где сходятся 9 модулей: `Math`, `Phases`, `Replay`, `Terrain`, `ManaField`, `Runes`, `Character`, `EventSystem`, `ECSSystem`.

## 1. Зачем и главная идея

Вся игра «Первое заклинание» **без окна и графики** — один объект `Simulation`: ландшафт, поле маны, ECS с персонажем и заклинаниями, генератор с сидом. Тесты, повторы (`--replay`), сеть (`Net`) и уровни (`Challenge`) запускают именно его.

Принцип: **мир меняется только командами внутри тика**. Состояние наружу — `const` (`terrain()`, `mana()`, `world()`, `spells()`), поэтому изменить мир мимо команд нельзя → запись всегда воспроизводима. `Simulation` удовлетворяет концепту `Replay::Simulatable` (`tick(span<Command>)`, `tick_number()`, `hashes()`), поэтому `Replay::Driver` и `Net::Lockstep` работают с ним без адаптеров.

## 2. Состав

| Часть | Что |
|---|---|
| Команды | `CommandType {Move=1, Jump=2, Cast=3, SetProgram=4}`; структуры `MoveCommand`, `JumpCommand`, `CastCommand`, `SetProgramCommand` с `encode()`/`decode()` в `Replay::Command`; хелперы `move_command`, `cast_command`, `set_program_command`; `register_commands(registry)`; `blob_reference(command)` |
| События | `TerrainEditedEvent` (границы правки, число чанков) |
| Настройка | `Config{seed, setup[], spawn_offset_x/z_m, max_custom_programs, mana, runes, character, player_mana, player_mana_regen, grimoire[3]}`, `SetupEdit{carve, center, radius}` |
| `Simulation` | `tick`, `hashes`, `provide_program`/`provide_blob`/`has_program`, `reload_spells`, `set_grimoire_slot`, `schedule()`, `take_dirty_chunks`, счётчики, доступ только на чтение к миру |

## 3. Как устроено

### 3.1 Тик — расписание фаз (`Phases::Schedule`, 60 Гц)
`commands → spells → terrain → mana → movement → events`
1. **commands** — каждая команда тика применяется (`apply`).
2. **spells** — `SpellSystem::tick` исполняет все активные заклинания **в порядке id**; мир они меняют только данными (`Effect`) в `m_effects`.
3. **terrain** — эффекты применяются **в порядке исполнения** (`carve_sphere`/`add_sphere`); результаты запоминаются для событий; буфер очищается.
4. **mana** — поле шагает **каждый 6-й тик** (`mana_step_period`, 10 Гц).
5. **movement** — `Character::step(world, terrain, config)`.
6. **events** — `TerrainEditedEvent` по каждой правке (если вызван `declare(bus)`); изменённые чанки уже стоят в очереди `take_dirty_chunks()`.
Новая фаза подключается `sim.schedule().insert_after("movement", "machines", fn)` без правки этого файла. Порядок фаз — часть детерминизма (хеши).

### 3.2 Команды
Формат — `Replay::Command` (16 байт). Кодирование:
- `Move`: `x = dx.raw`, `z = dz.raw` (Fixed); при применении длина > 1 → нормализуется; `Motor::wish` **остаётся**, пока не придёт новая `Move` (липкое направление — чтобы остановиться, шлют `Move{0,0}`).
- `Jump`: одноразовый запрос `Motor::jump`.
- `Cast`: `arg = slot | (source << 8)`, `x,y,z` — прицел (Fixed); нет заклинания в слоте → `++failed_casts`; иначе `SpellSystem::cast(..., normalize(aim), source)`.
- `SetProgram`: `arg = slot`, `x/y` — младшие/старшие 32 бита **хеша программы**; нет блоба с таким хешем или неверный слот → **отклоняется** (`rejected_programs++`), слот прежний — отказ одинаков у всех, симуляция не расходится.
Правила превращения ввода в команды: `float → Fixed` один раз на границе (`Math::quantize_direction`).

### 3.3 Блобы-программы
`provide_program(bytes)` — `decode_program` (граница доверия), `content_hash`, имя `#<хеш>`; повторная подача безвредна; число различных ограничено `max_custom_programs` (64). `provide_blob` — обёртка для `Replay::Driver` (ошибки игнорируются — команда будет отклонена). `has_program(hash)` — для ворот `Net::Lockstep`. Это **данные, а не состояние мира**: момент подачи значения не имеет, пока блоб пришёл до команды.

### 3.4 `Host` — мост к миру для рун
Внутренний класс `Simulation::Host : Runes::SpellHost`: `position` = глаза персонажа (`Character::eye`); `target` = луч по `terrain.raycast` (иначе точка на дальности); `density/draw` → `ManaField`; `take_personal/give_personal` → `Character::ManaPool` (с проверкой хватает ли).

### 3.5 Конструктор: уровни поверх сида
Мир строится из `seed`; затем применяются `Config::setup` (шар породы/пустоты) — **это часть настройки, а не команда**: в записи известна по номеру уровня. Игрок появляется в центре мира + `spawn_offset`, на `ground_height + 0,5 м`. Гримуар — `Config::grimoire` (по умолчанию `carve`, `raise`, `runaway`).

### 3.6 Хеши
`hashes()`: `terrain`, `mana`, `characters`, `spells`, `rng`, `tick` (6 из максимум 8 в `StateHashes`). `rng` — состояние `Math::Rng`, засеянного `seed` (симуляция его пока не расходует, но хеш уже в составе формата).

### 3.7 События и счётчики
`declare(bus)` объявляет модуль `SpellSim` и `TerrainEditedEvent` (бюджет 256/тик), а также события `Runes`. Счётчики для интерфейса/судьи: `casts`, `failed_casts`, `edits_applied`, `programs_set`, `rejected_programs`.

## 4. Как использовать

```cpp
SpellSim::Simulation sim(SpellSim::Config{.seed = 11});
sim.programs().add_text("carve", "TARGET\nPUSH 2\nCARVE\nHALT\n");

Replay::Driver driver(sim, session, &flight);                       // Simulation — Replay::Simulatable
driver.step({SpellSim::CastCommand{0, Runes::ManaSource::Personal, aim}.encode()});
Replay::StateHashes h = sim.hashes();                               // terrain, mana, characters, spells, rng, tick

const auto hash = driver.submit_blob(Runes::encode_program(program));          // правка редактора
live.push_back(SpellSim::set_program_command(1, hash));

sim.schedule().insert_after("movement", "machines", [&]{ step_machines(); });  // своя фаза
for (auto c : sim.take_dirty_chunks()) { /* перестроить меш чанка c */ }
```
Где это в проекте: `FirstSpell`, `RuneCell2`, `CoopScribe`, `Challenge::play` и `Referee`, `ModuleProof`, golden-тесты `SpellSim/tests/golden`.

## 5. Что менять осторожно

- **Любое изменение порядка фаз, формул, форматов команд меняет хеши** → golden-эталоны падают: `tools/update_golden.sh`, новые `.rec` коммитятся вместе с изменением.
- **Один персонаж.** Команды без идентификатора игрока управляют `m_player`. Мультиплеер = `player_id` в команде (формат `Command` — 16 байт, нужно решение: расширить или кодировать в `type`/`arg`).
- Известная ошибка из `Runes` (утечка маны при провале каста из окружения) проявляется **здесь**, потому что `Host::take_personal` списывает ману до `draw`; исправление затронет golden-сценарии.
- `Config::setup` и `spawn_offset` **не попадают в запись** — только в хеш мира; воспроизведение должно создавать `Simulation` с тем же `Config`.
- `provide_program` нельзя звать между `begin_tick` и `tick` с расчётом на порядок: правило «блоб до команды» держит `Replay` (журнал) и `Net` (ворота `ready()`).
- Состояние наружу — только `const`: не добавляй non-const геттеры мира — это дыра в детерминизме.
- Не забывай `declare(bus)` перед тиком, если нужны события — иначе `phase_events` просто выходит (`m_declared`).

## 6. Упражнения для переписывания

1. Добавь команду `Select` (выбор слота гримуара) — пройди весь путь: структура, `encode/decode`, `register_commands`, `apply`, хеш, golden-сценарий.
2. Сделай `player_id`: массив персонажей, команда с `arg`-полем игрока, `Character::step` по всем — как проверишь, что порядок по id не зависит от истории?
3. Вынеси применение эффектов в отдельную фазу «effects» и сравни хеши до/после (должны совпасть, если порядок тот же).
4. Добавь фазу «machines» (по образцу `insert_after("movement")`) — простую турель, стреляющую заклинанием; какие хеши ей нужны?
5. Исправь утечку маны и обнови golden: опиши, что изменилось в сценариях.

## 7. Тесты и примеры

`Modules/SpellSim/tests` (`SpellSimTests`: порядок тика, оба заклинания, дыра в тумане, бесконечный цикл, `SetProgram`, запись/повтор), `tests/golden/*.rec` + тест `Golden`, пример `01_headless_simulation.cpp`.
