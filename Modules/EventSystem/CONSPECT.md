# EventSystem — конспект для изучения и переписывания

Самый крупный «нерендерный» модуль (~3000 строк). Читай в таком порядке: `Core/` → `Storage/` → `Channel/` → `Bus/` → `Graph/`. Официальное описание — `docs/mainpage.md`; здесь — как всё это связано внутри. Зависимостей от других модулей нет.

## 1. Зачем модуль и главная идея

Модули движка (физика, магия, мир, UI…) **не вызывают друг друга**, а отправляют и читают **события** через шину. Шина не знает ни одного конкретного события: каждый модуль описывает свои события в публичном заголовке.

Три независимые оси (меняются по отдельности):

| Ось | Вопрос | Где |
|---|---|---|
| Схема | какие поля | `E::fields`, `EventSchema` |
| Раскладка | как лежит в памяти | `E::layout`: `AoS` / `SoA` |
| Доставка | когда видно и сколько живёт | `ChannelConfig::delivery`: `Stream` / `Coalesced` / `Scheduled` |

Модель времени: события, отправленные в тике N, **видны ровно в тике N+1** (двойной буфер). Следствия: порядок систем внутри тика не влияет на прочитанное; цепочка «событие → реакция → событие» растягивается по тикам и не может зациклить тик (важно для заклинаний, собранных игроками).

## 2. Карта файлов

| Папка | Файлы | Что |
|---|---|---|
| `Core/` | `Ids.hpp` | `Tick`, `EventRef`, `EventId`, `ModuleId` |
| | `Event.hpp` | `Layout`, `Field<Name, &E::f>`, `Fields<…>`, концепт `Event`, `event_id_v`, `field_index_v` |
| | `Schema.hpp` + `Schema.cpp` | `EventSchema`, `FieldDesc`, `FieldKind`, `validate_schema`, `schema_of<E>()` |
| | `FixedString.hpp`, `FNV1a.hpp`, `Error.hpp` | строка как параметр шаблона, хеш, `EventSystemError` |
| `Storage/` | `ColumnBuffer` | сырой выровненный массив одной колонки (аналог VBO) |
| | `EventBuffer` | набор событий одного типа в AoS или SoA |
| `Channel/` | `IChannel.hpp` | `Delivery`, `Domain`, `ChannelConfig`, `ChannelStats`, интерфейс |
| | `Channel.hpp/.cpp` | **единственная** реализация: буферы, политики, дорожки |
| `Bus/` | `EventBus` | каналы, тики/кадры, контракты модулей, дерево причин |
| | `EventWriter`, `EventReader` | типизированные «ручки» на горячем пути; `LaneWriter` |
| `Graph/` | `ModuleRegistry`, `EventGraph` | декларации модулей и граф зависимостей |

## 3. Как устроено

### 3.1 Описание события (`Core/Event.hpp`)
```cpp
struct VoxelChanged {
    std::int32_t x, y, z; std::uint32_t block;
    static constexpr std::string_view event_name = "world.voxel_changed";   // источник EventId
    static constexpr EventSystem::Layout layout = EventSystem::Layout::SoA; // по умолчанию AoS
    using fields = EventSystem::Fields<Field<"x", &VoxelChanged::x>, /* … все поля … */>;
};
```
- Концепт `Event`: класс, trivially copyable, default-constructible, есть `event_name`, `fields`, и **все** `Field` принадлежат именно этому типу (поле чужого события — ошибка компиляции, потому что поле — указатель на член).
- `EventId = FNV-1a(event_name)` — от **имени события**, а не C++-типа: переименование структуры не ломает сохранения/сеть. Коллизия хешей ловится при регистрации.
- Порядок полей в `Fields` = порядок колонок SoA и полей схемы. Забытое поле → схема отклоняется (`validate_schema` проверяет покрытие всех байт, кроме padding).

### 3.2 Схема (`Schema.hpp`)
`EventSchema{name, id, size, alignment, layout, fields}` — рантайм-описание («VAO»). Для C++-событий строится `schema_of<E>()` (статик, смещения полей берутся с реального объекта-пробы, поэтому совпадают с компилятором). Для скриптов/модов схему можно собрать руками и `register_schema`. `FieldKind` — скаляры; всё остальное `Opaque` (копируется как байты).

### 3.3 Хранилище
- `ColumnBuffer`: сырая память под `capacity` элементов по `stride`; начало выровнено минимум на 64 байта (колонки не делят кэш-линии). Размер хранит владелец — у всех колонок буфера он общий.
- `EventBuffer`: AoS = одна колонка (`stride = sizeof(E)`), SoA = колонка на поле. Два уровня доступа:
  - **типизированный** (`push<E>`, `events<E>`, `column<E,&E::f>`, `field`, `get`) — прямые указатели, `if constexpr` по раскладке; соответствие `E` схеме проверяется `assert`ом (в шине — один раз при выдаче писателя/читателя);
  - **сырой** (`push_raw`, `read_raw`, `field_data`) — для скриптов и инструментов.
  - `swap` — O(1); `append(source, first, count)` — memcpy на колонку (так сливаются дорожки); `overwrite` — для Coalesced; `erase_front` — для Scheduled.
  - Причины (дерево причин): при `set_tracing(true)` рядом хранится `vector<uint64_t>` — по причине на событие.
- `events<E>()` только для AoS, `column<E,&E::f>()` только для SoA — это `static_assert`, то есть неверный выбор не соберётся.

### 3.4 `Channel` — сердце модуля
```
 писатели (главный поток) ─▶ pending ─┐
 дорожки потоков 0..N     ─▶ lanes[i] ─┤ advance(): дорожки → pending (по порядку, бюджет)
 emit_after (Scheduled)   ─▶ future  ─┤             политика: pending/future → ready
                                       ▼
                                     ready ─▶ читатели (видно весь следующий момент)
```
**Горячий путь одинаков у всех политик:** писатель кладёт в `pending`, читатель смотрит `ready`. Политика работает только в `advance()`:
- `Stream`: `ready.clear(); ready.swap(pending)` — O(1), без копий. Адреса `ready()` и `pending()` у канала неизменны (меняется содержимое), поэтому `EventReader` держит указатель навсегда.
- `Coalesced`: идём по `pending`, ключ — поле `coalesce_field` (≤ 8 байт); есть ключ → `overwrite` (последнее значение **на месте первого**), нет → `append`. Считает `total_coalesced`.
- `Scheduled`: `emit_after(e, N)` кладёт в `future` с `due = time + N` (0 → 1). В `advance` созревшие (`due ≤ time`) идут в `ready` **первыми** (они старше), затем обычные события; остальные остаются в `future` (через временный `m_kept`).
- **Бюджет** `max_events_per_tick`: `emit` возвращает `false` и считает `total_dropped`; при слиянии дорожек лишнее отбрасывается **с конца** детерминированно.
- **Дорожки потоков:** `open_lanes(n)` (главный поток, до работы) → каждый кусок `parallel_for` пишет только в свою дорожку (`EventBuffer` той же схемы, `alignas(64)`) без замков; в `advance()` дорожки дописываются в `pending` по порядку номеров → результат одинаков при любом числе потоков. События главного потока идут раньше дорожек.
- Политику и домен **нельзя сменить** после создания (`configure` бросает).
- `time()` — сколько раз канал сменил момент; `ref(i)` строит `EventRef(time, channel, index)`.

### 3.5 `EventBus`
- Владеет `unique_ptr<Channel>` по одному на тип; `unordered_map<EventId, index>`.
- `register_event<E>(config)`: валидирует схему; повторная регистрация возвращает существующий канал (и применяет `config`, если передан); одно имя с разной схемой или коллизия хеша — `EventSystemError`.
- `writer<E>()` / `reader<E>()` — проверяют, что схема `E` совпадает с зарегистрированной; **выдать один раз, хранить в системе** (поиск канала происходит только здесь). Версии `writer<E>(module)` дополнительно проверяют контракт `produces<E>()`.
- Время: `advance_tick()` сдвигает каналы домена `Tick`, `advance_frame()` — домена `Frame` (кадровые каналы живут и на паузе). `Core` вызывает их сам.
- Дерево причин: у канала с `trace=true` каждое событие хранит `EventRef` причины; после `advance` шина пишет `TraceRecord` в **кольцевой журнал** (по умолчанию 65 536). `cause_chain` (до 256 звеньев), `effects`, `trace_tree` — линейный поиск по журналу, годятся для отладки, не для горячего пути.
- Исключения только на холодных путях (регистрация, выдача ручек); `emit`/чтение/`advance` не бросают.
- Шина **не потокобезопасна**; параллельность — только через дорожки.

### 3.6 Контракты модулей и граф
`bus.declare_module("Physics").produces<Hit>().consumes<Input>()` — заодно регистрирует каналы. `EventGraph` (снимок): `to_text()`, `to_dot()` (Graphviz), `unproduced_events()` / `unconsumed_events()` / `orphan_events()`, `module_edges()`, `module_order()` (топологический порядок и циклы). Это **декларативная** документация системы: если модуль пишет событие, которого не объявил, `writer<E>(module)` бросит.

## 4. Как использовать

```cpp
EventSystem::EventBus bus;
auto physics = bus.declare_module("Physics").produces<Hit>({.reserve = 256, .max_events_per_tick = 4096});
auto combat  = bus.declare_module("Combat").consumes<Hit>().produces<Damage>();

auto hits_out = bus.writer<Hit>(physics);      // один раз
auto hits_in  = bus.reader<Hit>(combat);

// тик N: physics пишет, combat читает события тика N-1
hits_out.emit(Hit{...});
for (const Hit& h : hits_in.events()) {...}   // AoS; для SoA — hits_in.column<&Hit::amount>()
bus.advance_tick();

// параллельная запись без замков
auto lanes = hits_out.lanes(JobSystem::chunk_count(n, grain));
JobSystem::parallel_for(jobs, n, grain, [&](size_t b, size_t e, size_t c){ lanes.emit(c, Hit{...}); });
```
Политики: `bus.register_event<HealthChanged>({.delivery = Delivery::Coalesced, .coalesce_field = coalesce_key<HealthChanged, &HealthChanged::entity>()})`; `poison.emit_after(PoisonTick{e}, 3)` для Scheduled.

Где это в проекте: `RuntimeSystem::Runtime::bus()` создаёт шину для всех модулей; `Core::App` публикует платформенные события; `SpellSim` объявляет `TerrainEditedEvent` и др.; `EventLog::BusAdapter` пишет события шины в журнал; `Siege` использует дорожки.

## 5. Что менять осторожно

- **Горячий путь без виртуальных вызовов** — главная цель дизайна. Если добавишь новую политику, держи её целиком в `advance()`; не трогай `emit`/`reader`.
- Адреса `Channel::pending()/ready()` обязаны быть неизменными (читатели хранят указатель). `swap` меняет содержимое, а не объекты.
- `EventRef`: время — 32 бита, канал — 12 бит (≤ 4096 каналов), индекс — 20 бит (≤ ~1 млн событий на канал за момент). Превышение молча обрежется маской — следи за бюджетами.
- `Coalesced` теряет порядок: событие остаётся на месте **первого** с этим ключом, а значение берёт у последнего.
- `static_assert`ы раскладки (`events` только AoS, `column` только SoA) — защита от тихой ошибки; не ослабляй.
- Журнал причин — линейный поиск; для длинных сессий уменьшай `set_trace_capacity` или включай `trace` только на нужных каналах.
- Схема события = формат на диске/в сети: переименование `event_name` меняет `EventId`.

## 6. Упражнения для переписывания

1. Напиши свою шину «на `std::vector<std::any>`» и сравни по `BM_Emit_*` — увидишь цену типобезопасной горячей дороги.
2. Реализуй `Delivery::Latest` (в `ready` только одно последнее событие канала) как четвёртую политику — какие части `Channel` придётся тронуть? (должна хватить `advance`).
3. Сделай `EventReader::since(time)` — читать события за несколько тиков (потребует хранить историю).
4. Замени линейный поиск в `cause_chain` на индекс `unordered_map<EventRef,size_t>` и измерь.
5. Добавь рантайм-событие через `register_schema` (скриптовое) и запиши его в `EventLog` без C++-типа.

## 7. Тесты и примеры

`Modules/EventSystem/tests` (14 файлов — самое тщательно покрытое место), 5 примеров, бенчмарки `BM_Emit_*`.
