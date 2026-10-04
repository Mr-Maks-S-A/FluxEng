# Конспект 16 — состояние после слияния и варианты более чистого API модулей

Конспект 15 описал `Net`, `Challenge`, блобы и `SetProgram`. Здесь две вещи: **что изменилось в репозитории** (слияние веток) и **разбор того, как модули стыкуются в игровом коде**
с вариантами API. Код в этом конспекте не менялся — это предложения; выбрать вариант предстоит отдельно.

## 1. Состояние репозитория (2026-10-04)

- В `main` влиты две ветки; **на сервере и локально осталась одна ветка `main`** (`6cabf8d`), остальные удалены после проверки, что они целиком в `main`.
  - `exciting-curie`: `Net`, `Challenge`, `CoopScribe`, `SetProgram` (конспект 15) и **ModuleProof** — игра-проверка, которая линкуется с зонтичной целью `engine::FluxEng` и по одной проверке на тик прогоняет каждый модуль.
  - `stoic-ramanujan`: **`RuntimeSystem`** — ядро без графики и окна (`Runtime`, `Module`, `FixedStep`); `Core::App` построен поверх него. Нужен выделенному серверу и тестам.
- Конфликты (два): `tools/check_modules.sh` (список модулей) и `Tests/CMakeLists.txt` (линковка `IntegrationTests`) — разрешены объединением сторон.
- Проверка слияния: сборка с нуля (Release, gcc 14) и **678 из 678 тестов**. Предупреждение `-Wshadow` про `Tick` (`EventSystem/Channel/IChannel.hpp:46` против `Ids.hpp:24`) — старое, не из слияния.
- Модулей теперь **22**: Challenge, Character, Core, ECSSystem, EventLog, EventSystem, JobSystem, ManaField, Math, MemorySystem, Net, Phases, RendererSystem, Replay, RuneEditor, Runes,
  RuntimeSystem, SpellSim, Terrain, WindowSystem, WorldRender (+ зонтичная цель).

## 2. Где API неудобен: что видно в реальных местах использования

Смотрел на `CoopScribe` (`Player::frame`), `RuneCell2`, `ModuleProof` и заголовки `Replay`, `Net`, `Challenge`, `SpellSim`, `RuntimeSystem`.

| # | Наблюдение | Где |
|---|---|---|
| А | **Три копии одного цикла шага**: `Replay::Driver::step`, `Player::frame` (Lockstep), `RuneCell2` — везде «взять команды → доставить блобы → `sim.tick` → `observe` → хеши». Различается только источник команд. | `Replay.hpp:310`, `coop_scribe/main.cpp:76-103` |
| Б | **Блоб + команда — ручная пара**: `Lockstep::submit_blob` → хеш → `set_program_command(slot, hash)`; на приёме `blob_reference` → `has_program` → `provide_program(*net.blob(hash))`. Забыл любой шаг — тихое расхождение или «мёртвый» блоб. | `coop_scribe/main.cpp:84-96` |
| В | **Команды кодируются вручную**: `MoveCommand`/`CastCommand` с `encode/decode` + отдельный `register_commands` со схемой + `blob_reference`-хук. Три места на одну команду; новая команда = правка трёх мест. | `SpellSim.hpp` |
| Г | **`Lockstep` — шесть вызовов в строгом порядке**: `pump` → `needs_input` → `submit` → `ready` → `advance` → `report_hashes`. Порядок знает только документация. | `Lockstep.hpp` |
| Д | **Ошибки трёх видов**: `expected<T, std::string>` (Replay), `RuntimeError` исключением (RuntimeSystem), `std::string failure()` + `desync()` (Net), `Report` (Runes). Нельзя обработать единообразно. | — |
| Е | **Строки как идентификаторы**: `depends_on("Physics")`, `StateHashes::add("terrain", …)`, `hashes().at("terrain")`, `find_level("citadel")`. Опечатка видна только на запуске. | `Runtime.hpp`, `SpellSim.hpp` |
| Ж | **Нет единых заголовков модулей**: у `Net`, `Challenge`, `Replay`-подсистем нет `<Net/Net.hpp>`; ModuleProof подключает ~25 заголовков. У `Core`, `RuntimeSystem`, `SpellSim` зонтик есть. | `Sandbox/module_proof` |
| З | **Сырые указатели в параметрах**: `play(level, solution, Replay::Session* session = nullptr)`, `FlightRecorder*`. Не видно, кто владеет и обязателен ли аргумент. | `Play.hpp`, `Replay.hpp` |

## 3. Варианты

Для каждого — «как сейчас», вариант **A** (минимальный, обратно совместимый) и **B** (смелее). Рекомендация отмечена.

### 3.1 Единый источник команд (наблюдение А)

Сейчас:
```cpp
// Replay
Replay::Driver<SpellSim::Simulation> driver(sim, session);   driver.step(live);
// Net — свой цикл руками (Player::frame)
```
**A. Концепт `CommandSource` — Driver принимает что угодно, что умеет отдать команды тика.**
```cpp
template<class S> concept CommandSource = requires(S& s, std::uint32_t t) {
    { s.poll(t) } -> std::same_as<std::optional<std::span<const Replay::Command>>>; // nullopt — тик ещё не готов
};
Replay::Stepper<SpellSim::Simulation, Net::Lockstep> stepper(sim, net);   // тот же шаг, источник — сеть
Replay::Stepper<SpellSim::Simulation, Replay::Session> stepper(sim, session);
while (stepper.step()) { referee.observe(sim); }
```
`Lockstep::poll` склеивает `pump + ready + advance`, `Stepper` сам доставляет блобы и вызывает `report_hashes`. Запись поверх сети (`Session` как «наблюдатель» второго параметра) — бесплатно.

**B. Источники составляются:** `Recorded{Live{}}`, `Recorded{Net{}}` — запись любой партии, включая сетевую, как декоратор. Красивее, но декораторы требуют общей схемы владения; пока рано.

**Рекомендация: A.** Убирает три копии цикла и закрывает Г для типичного случая. Низкоуровневый `Lockstep` остаётся для тех, кому нужен ручной контроль.

### 3.2 Блоб как часть команды (наблюдение Б)

Сейчас — две сущности и ручная доставка. Хочется, чтобы **команда с вложением** была одной вещью:
```cpp
// A. Тип команды сам знает про вложение — игроку остаётся один вызов
net.submit(SpellSim::SetProgram{slot, program});     // внутри: encode_program → submit_blob → команда с хешем
driver.submit(SpellSim::SetProgram{slot, program});  // то же для записи и повтора

// B. Общий интерфейс «отправитель»: Driver, Lockstep и Session реализуют CommandSink
template<class C> concept Attachable = requires(const C& c) { { c.attachment() } -> std::convertible_to<std::span<const std::byte>>; };
sink.send(command);   // sink сам решает: в запись, в сеть, в симуляцию
```
На приёме: `Stepper` (3.1) проверяет `blob_reference` и `provide_program` сам.

**Рекомендация: A как следствие 3.1**, B — только если появится третий вид вложений (кроме программ).

### 3.3 Типизированные команды вместо `encode/decode` (наблюдение В)

Сейчас на команду: структура + `encode` + `decode` + запись в `register_commands`. События шины уже решены лучше — `Fields<Field<"lo_x", &T::lo_x>, …>`. Сделать так же:
```cpp
struct MoveCommand {
    Math::Fixed dx{}, dz{};
    static constexpr std::uint16_t type_id = 1;
    static constexpr std::string_view name = "move";
    using fields = Replay::Fields<Replay::Fixed<"dx", &MoveCommand::dx, Slot::X>, Replay::Fixed<"dz", &MoveCommand::dz, Slot::Z>>;
};
using SpellCommands = Replay::CommandSet<MoveCommand, JumpCommand, CastCommand, SetProgramCommand>;
// encode/decode/схема/format выводятся из fields; register_commands(registry) → SpellCommands::register_in(registry)
auto c = Replay::encode(MoveCommand{dx, dz});
std::visit(overloaded{...}, Replay::decode<SpellCommands>(c));   // std::variant, неизвестный тип → ошибка
```
Выигрыш: новая команда — одна структура; схема файла записи не может разойтись с кодом (сейчас расходится тихо). Цена: шаблонный слой (в стиле, уже принятом в `EventSystem`), `sizeof(Command) == 16` остаётся.
**Рекомендация: делать**, но после 3.1, чтобы не переписывать дважды.

### 3.4 `Lockstep`: шаг за один вызов (наблюдение Г)

```cpp
// A. Один «кадр сети» с колбэками; порядок вызовов зашит внутри
net.frame({
    .input   = [&](std::vector<Command>& out) { policy(sim, out); },   // зовётся, только когда нужен ввод
    .step    = [&](std::span<const Command> cmds) { sim.tick(cmds); },  // зовётся, только когда тик готов
    .hashes  = [&] { return sim.hashes(); },
});
// B. Состояние как значение: auto s = net.poll(); if (s.needs_input) …; if (s.tick) …  — без колбэков, но снова порядок на совести пользователя
```
**Рекомендация: A** (с 3.1 это и есть `Stepper`; колбэки нужны тем, кто Stepper не использует).

### 3.5 Единый тип ошибки (наблюдение Д)

```cpp
// A. Один маленький тип для всех модулей; конвертируется в строку, но несёт код
struct Error { enum class Code : std::uint8_t { Io, Format, Version, Corrupt, Rejected, Desync, Stalled } code; std::string detail; };
template<class T> using Result = std::expected<T, Error>;
Result<Recording> load(const std::string&);       // вместо expected<Recording, std::string>
if (auto r = Recording::load(p); !r && r.error().code == Error::Code::Corrupt) { /* чинить */ }
// B. Каждый модуль — свой enum (Replay::Error, Net::Error): точнее, но нет общей обработки наверху.
```
Где-то должен жить общий `Error` — разумно в `Math` (там уже `content_hash`, `crc32c`) или новом микромодуле `Base`. `RuntimeError` оставить исключением — это нарушение контракта, а не ожидаемый отказ.
**Рекомендация: A.** Начать с `Replay` и `Net` (там `std::string`), `Runes::Report` не трогать — это отчёт, а не отказ.

### 3.6 Идентификаторы без строк (наблюдение Е)

```cpp
// A. Тип-тег вместо строки для зависимостей модулей — опечатка не соберётся
struct Combat final : RuntimeSystem::Module { Combat() { depends_on<Physics>(); } };   // имя берётся из Physics::module_name
// Б. Именованные хеши: перечисление подсистем в самой симуляции
enum class Subsystem : std::uint8_t { Terrain, Characters, Mana, Spells };
hashes.set(Subsystem::Terrain, h);   hashes.at(Subsystem::Terrain);   // имя строкой — только для вывода и файла
```
Для `StateHashes` важно, что **имена попадают в файл записи** (самоописываемость): значит, остаются строкой в формате, но в коде — `constexpr`-константа: `SpellSim::sub::terrain`.
**Рекомендация: A для модулей** (дёшево: шаблонная перегрузка), **для хешей — константы** (`inline constexpr std::string_view`), полноценный enum — не нужен.

### 3.7 Единые заголовки модулей (наблюдение Ж)

Добавить зонтики `<Net/Net.hpp>`, `<Challenge/Challenge.hpp>`, `<EventLog/EventLog.hpp>`, `<RuneEditor/RuneEditor.hpp>` (у `Replay`, `Core`, `RuntimeSystem`, `SpellSim` один главный заголовок уже есть).
Цель: ModuleProof и игры подключают **по одной строке на модуль**; `tools/check_modules.sh` может проверять, что зонтик самодостаточен (уже делает для других). Три строки каждый, без риска.
**Рекомендация: делать сразу.**

### 3.8 Опции вместо сырых указателей (наблюдение З)

```cpp
// A. Структура опций с именованными полями — читается и расширяется без поломки вызовов
struct PlayOptions { Replay::Session* session = nullptr; Replay::FlightRecorder* recorder = nullptr; };
Outcome play(const Level&, const Solution&, PlayOptions = {});
play(level, solution, {.session = &session});
// B. Ссылка-обёртка: std::optional<std::reference_wrapper<Session>> — шумно, не стоит.
```
**Рекомендация: A.**

### 3.9 Сервер и клиент на одном `Runtime` (из RuntimeSystem)

Теперь `Net`, `Replay::Session` и `Referee` можно сделать **модулями `Runtime`**, а не объектами, которые игра держит сама:
```cpp
RuntimeSystem::Runtime rt({.ticks_per_second = 60.0, .lockstep = true});
rt.add<SimModule>(level);        // владеет Simulation, команды берёт у CommandSource
rt.add<NetModule>(transport, cfg);
rt.add<RefereeModule>(level);    // подписан на шину: тик симуляции → observe
rt.initialize();  rt.run({.max_ticks = 600, .realtime = false});
```
Порядок тика даёт `depends_on`, а не ручная раскладка вызовов в `Player::frame`. Клиент (`Core::App`) добавляет те же модули плюс окно и рендер — сервер и клиент **буквально один и тот же набор**. Это путь к выделенному серверу `CoopScribe`.
**Рекомендация: после 3.1–3.4** — модули-обёртки оправданы, когда сам шаг уже один.

## 4. Предлагаемый порядок

1. **3.7** (зонтики) и **3.8** (опции) — мелкие, без риска, дают немедленно чище ModuleProof и вызовы `play`.
2. **3.1 + 3.2 + 3.4** одним заходом (`Stepper`, `CommandSource`, `net.frame`) — убирает три копии цикла и ручные блобы; проверка — `CoopScribe`, `Integration.cpu: …сетевая партия`, golden-эталоны (хеши не должны измениться).
3. **3.3** (типизированные команды) — формат записи не меняется: проверить, что golden-файлы читаются как раньше.
4. **3.5** (`Error`) — по модулям, начиная с `Replay`/`Net`.
5. **3.6**, **3.9** — по мере надобности.

**Инвариант для всех шагов:** файлы записей и хеши симуляции не меняются; golden-эталоны проходят без обновления.

## 5. Открытое из конспекта 15 (не решено)

Несколько персонажей и идентификатор игрока в команде; UDP-транспорт и сессия (таймаут, исключение пира); функции и переменные в Runes; **утечка маны при провале каста из окружения** (`Runes::SpellSystem::run`, меняет golden); UI уровней; больше уровней.
