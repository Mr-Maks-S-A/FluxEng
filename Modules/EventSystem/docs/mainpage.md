# EventSystem — шина событий FluxEng {#mainpage}

EventSystem — связующий модуль движка. Модули (физика, магия, мир, UI…) не вызывают
друг друга напрямую: они отправляют и читают **события** через шину. Шина не знает
конкретных событий; каждый модуль описывает свои события в публичном заголовке-контракте.

Подключение: `#include <EventSystem/EventSystem.hpp>`, CMake-цель `engine::EventSystem`.

## Три независимые оси

| Ось | Вопрос | Где задаётся |
|---|---|---|
| **Схема** | Какие поля у события | `E::fields`, EventSystem::EventSchema |
| **Раскладка** | Как лежит в памяти | `E::layout`: EventSystem::Layout::AoS / EventSystem::Layout::SoA |
| **Доставка** | Когда видно и сколько живёт | EventSystem::ChannelConfig::delivery, EventSystem::Delivery |

Оси меняются независимо. Все политики живут в одном классе EventSystem::Channel:
запись и чтение у них общие, политика решает только, что станет видно при смене момента.
Поэтому смена политики не меняет ни событий, ни систем.

Аналогия с OpenGL: EventSystem::ColumnBuffer — это VBO (байты без типа),
EventSystem::EventSchema — VAO (как эти байты читать), а раскладка решает,
лежат ли атрибуты interleaved (AoS) или в отдельных буферах (SoA).

## Как описать событие

```cpp
struct VoxelChangedEvent {
    std::int32_t  x, y, z;
    std::uint32_t block;

    static constexpr std::string_view event_name = "world.voxel_changed"; // источник EventId
    static constexpr EventSystem::Layout layout  = EventSystem::Layout::SoA; // по умолчанию AoS
    using fields = EventSystem::Fields<
        EventSystem::Field<"x", &VoxelChangedEvent::x>,
        EventSystem::Field<"y", &VoxelChangedEvent::y>,
        EventSystem::Field<"z", &VoxelChangedEvent::z>,
        EventSystem::Field<"block", &VoxelChangedEvent::block>>;
};
```

Правила (проверяются концептом EventSystem::Event и функцией EventSystem::validate_schema()):

- структура trivially copyable: никаких `std::string`, `std::vector`, указателей-владельцев;
- в `fields` перечислены **все** члены структуры — забытое поле даёт ошибку при регистрации;
- поле описывается указателем на член, поэтому поле чужого события не компилируется;
- ID строится из `event_name`, а не из имени C++-типа: переименование структуры не ломает сохранения и сеть.

## Модель времени: тики и двойной буфер

Шина считает время в **тиках симуляции** (EventSystem::Tick), а не в кадрах рендера.
Политика EventSystem::Delivery::Stream:

```
тик N:    системы пишут  → pending        системы читают ← ready (события тика N-1)
advance_tick():  ready.clear(); swap(ready, pending)
тик N+1:  событие из тика N видно всем читателям ровно один тик
```

Следствия:

- порядок систем внутри тика не влияет на то, что они прочитают;
- цепочка «событие → реакция → событие» растягивается по тикам и не может зациклить тик
  (важно для заклинаний, которые собирают игроки);
- система, которая не читает канал в каком-то тике, пропускает события этого тика;
- в устойчивом режиме аллокаций нет: буферы переиспользуются.

**Бюджет** (EventSystem::ChannelConfig::max_events_per_tick) ограничивает число событий
за тик; лишние отбрасываются и учитываются в EventSystem::ChannelStats::total_dropped.

## Политики доставки

| Политика | Что видно после смены момента | Зачем |
|---|---|---|
| `Stream` | всё, что отправлено в прошлом моменте (swap, O(1)) | попадания, столкновения, команды |
| `Coalesced` | одно событие на ключ: последнее значение на месте первого | «здоровье изменилось», «чанк изменён» |
| `Scheduled` | событие через `delay` моментов (`emit_after`) | яд, перезарядка, отрастающие деревья |

```cpp
bus.register_event<HealthChanged>({.delivery = Delivery::Coalesced,
                                   .coalesce_field = coalesce_key<HealthChanged, &HealthChanged::entity>()});
poison.emit_after(PoisonTick{e}, 3);   // Scheduled: видно через 3 тика; emit == emit_after(1)
```

Ключ слияния — поле до 8 байт; политику и домен канала нельзя сменить после создания.

## Домены времени: Tick и Frame

Канал принадлежит одному домену (EventSystem::ChannelConfig::domain). Каналы `Tick` сдвигает
EventSystem::EventBus::advance_tick() (симуляция, фиксированный шаг), каналы `Frame` —
EventSystem::EventBus::advance_frame() (каждый кадр: интерфейс, ввод, отладка). На паузе тиков нет,
а кадровые каналы продолжают жить. Core вызывает `advance_frame()` каждый кадр.

## Дорожки потоков: параллельная запись без мьютексов

```cpp
auto lanes = writer.lanes(JobSystem::chunk_count(count, grain));   // главный поток, до работы
JobSystem::parallel_for(jobs, count, grain, [&](std::size_t b, std::size_t e, std::size_t chunk) {
    for (std::size_t i = b; i < e; ++i)
        if (hit(i)) lanes.emit(chunk, Hit{...});                     // ни мьютексов, ни атомиков
});
```

- Дорожка — обычный EventBuffer той же схемы и раскладки: у SoA-события в ней те же колонки.
- Каждая дорожка в своей кэш-линии (`alignas(64)`): потоки не делят даже счётчики.
- При смене момента дорожки сливаются в pending **в порядке номеров** (memcpy на колонку):
  результат одинаков при 1 и 16 потоках. События главного потока идут раньше дорожек.
- Бюджет применяется при слиянии: лишнее отбрасывается с конца, тоже детерминированно.
- Память дорожек переиспользуется между тиками.

## Дерево причин

С `ChannelConfig::trace = true` событие хранит причину — EventSystem::EventRef события,
которое его вызвало (`writer.emit(e, reader.ref(i))`). EventRef = (момент, канал, индекс) —
детерминирован, одинаков в повторах. Шина ведёт кольцевой журнал и отвечает на вопросы:
`cause_chain(ref)` (откуда), `effects(ref)` (что вызвало), `trace_tree(ref)` (дерево текстом).
Каналы без trace не платят ничего.

## Контракты модулей и граф зависимостей

```cpp
EventSystem::EventBus bus;
auto physics = bus.declare_module("Physics").produces<CollisionEvent>();
auto combat  = bus.declare_module("Combat").consumes<CollisionEvent>().produces<DamageEvent>();

auto out = bus.writer<CollisionEvent>(physics); // OK
auto bad = bus.writer<DamageEvent>(physics);    // EventSystemError: не объявлено

EventSystem::EventGraph graph = bus.build_graph();
graph.to_text();            // дерево «модуль → события → потребители»
graph.to_dot();             // Graphviz
graph.unconsumed_events();  // кто-то пишет, никто не читает
graph.module_order();       // порядок модулей и циклы
```

## Два уровня доступа

- **Типизированный** (EventSystem::EventWriter, EventSystem::EventReader) — для C++-систем.
  Писатель и читатель получают один раз; поиск канала и проверка схемы происходят только тогда.
  Горячий путь без виртуальных вызовов и без поиска.
- **Сырой** (EventSystem::IChannel::emit_raw, EventSystem::EventBuffer::field_data) — для скриптов,
  модов и инструментов, которые знают только EventSystem::EventSchema. События можно
  описать в данных и зарегистрировать через EventSystem::EventBus::register_schema().

## Ошибки

- Регистрация и получение писателей/читателей бросают EventSystem::EventSystemError.
- Горячий путь (`emit`, чтение, `advance_tick`) исключений не бросает; нарушения
  контрактов ловит `assert` в debug-сборке.
- Ошибки использования API по возможности ловятся при компиляции
  (`column()` у AoS-события, поле чужого события, нетривиальный тип).

## Ограничения беты

- Регистрация, `lanes()` и смена момента — только из главного потока; параллельно — только запись в дорожки.
- Дорожки копируются при слиянии (запись событий без вычислений упирается в память, см. бенчмарки).
- Коллизии хешей имён обнаруживаются при регистрации, но не разрешаются.

## Примеры

- `01_basic_stream.cpp` — минимальный цикл тиков.
- `02_soa_voxels.cpp` — массовые SoA-события и бюджет.
- `03_module_graph.cpp` — контракты модулей и граф зависимостей.
- `04_runtime_schema.cpp` — события без C++-типов (скрипты, моды, инспектор).
- `05_lanes_policies_causes.cpp` — дорожки потоков, Coalesced/Scheduled и дерево причин.

## Сборка, тесты, бенчмарки

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DEVENTSYSTEM_SANITIZE=ON
cmake --build build
ctest --test-dir build -R EventSystem          # юнит-тесты, compile-fail тесты, примеры
cmake --build build --target EventSystemDocs   # документация (нужен Doxygen)

cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target EventSystemBenchmarks
build-release/bin/EventSystemBenchmarks
```
