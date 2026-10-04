# RuntimeSystem — конспект для изучения и переписывания

`src/include/RuntimeSystem/{Runtime,Module,FixedStep,RuntimeSystem}.hpp` и `src/code/Runtime.cpp` (~630 строк). Зависит от `EventSystem`, `JobSystem`, `MemorySystem`. Ни окна, ни GPU.

## 1. Зачем и главная идея

Это **ядро игры без графики**: шина событий + задачи + память тика/кадра + фиксированный шаг + набор модулей с жизненным циклом. То, что нужно и клиенту, и **выделенному серверу**. `Core::App` (клиент) — это `Runtime` плюс окно, рендер и ввод; сервер использует `Runtime` как есть.

Идея: игровая логика — **модули** (`Module`), которые знают только `Runtime`. Тогда один и тот же модуль работает и в клиенте, и на сервере, и в тесте без окна.

## 2. Карта файлов

| Файл | Что |
|---|---|
| `Module.hpp` | базовый класс `Module`: `name`, `depends_on`, хуки `declare/init/frame/tick/shutdown` |
| `FixedStep.hpp` | `FixedStep`: сколько тиков выполнить за кадр, `alpha`, пауза, скорость, lockstep |
| `Runtime.hpp/.cpp` | `Runtime`, `RuntimeConfig`, `RunOptions`, `ModuleStats`, `Phase`, `RuntimeError` |

## 3. Как устроено

### 3.1 Жизненный цикл модуля
```
Runtime::initialize():  порядок по зависимостям → declare() ВСЕХ → init() каждого
каждый кадр:            frame() всех       (ввод, интерполяция; идёт и на паузе)
каждый тик:             tick() всех → обратный вызов игры → bus.advance_tick() → смена арен
Runtime::shutdown():    shutdown() в ОБРАТНОМ порядке init
```
Гарантии (они же — места, где легко сломать при переписывании):
- `declare()` всех модулей идёт **до первого `init()`** — к моменту `init` контракты всех событий известны, можно брать писателей/читателей.
- `shutdown()` вызывается **только у модулей, чей `init()` завершился успешно** (счётчик `m_initialized`); `noexcept`; ровно один раз, даже при исключении в тике и при простом уничтожении `Runtime`.
- Зависимость инициализируется раньше зависимого и завершается позже.
- Деструктор `~Runtime`: `shutdown()`, затем модули уничтожаются в обратном порядке; поле `m_modules` объявлено **последним**, чтобы модули умирали раньше шины, задач и арен, которыми пользуются.

### 3.2 Порядок модулей
`sort_modules()` — алгоритм Кана с выбором **наименьшего номера регистрации** среди готовых: зависимости раньше, остальное — в порядке `add()`. Это делает порядок детерминированным. Ошибки (дубликат имени, неизвестная зависимость, цикл с перечислением участников) — `RuntimeError`, **до** любого хука; фаза остаётся `Created`. Зависимость объявляется строкой: `depends_on("Physics")` в конструкторе.

### 3.3 Фазы `Runtime`
`Created → Initializing → Running → ShuttingDown → Stopped`. `add` — только в `Created`; `tick/update/begin_frame/run` — только в `Running`; иначе `RuntimeError` с именами фаз. Если `initialize()` бросил исключение, модули откатываются (`rollback`), фаза `Stopped`, исключение летит дальше. **Повторно использовать `Runtime` нельзя.**

### 3.4 Время
- `FixedStep`: `advance(frame_seconds)` накапливает `frame·speed`, выдаёт целое число тиков; `max_ticks_per_frame` (по умолчанию 16) — защита от «спирали смерти»: при упоре сбрасывает накопитель (лучше замедлиться, чем зависнуть). `paused` → 0 тиков, но кадры идут. `lockstep` → ровно 1 тик на кадр без учёта времени (детерминированные прогоны). `alpha()` — доля пути до следующего тика для интерполяции отрисовки.
- `begin_frame(dt)`: сброс арены кадра, `Module::frame` у всех. **Не** продвигает шину.
- `update(dt)`: `bus.advance_frame()` (кадровые каналы живут на паузе), затем `step.advance` и нужное число `tick()`. Клиент вызывает `begin_frame` и `update` сам.
- `tick()`: `tick()` модулей → `set_tick_callback` (код игры) → `bus.advance_tick()` → `tick_memory.swap()` (память тика N читается в N+1 через `previous_tick_arena`).
- `run(options)`: цикл без окна. `realtime=false` — тики подряд (тесты и симуляция вперёд); `true` — по часам, `dt` обрезан до 0.25 с, между тиками `sleep_for` (сервер не греет процессор). Остановка — `request_stop()` (атомарный флаг, можно из другого потока/обработчика сигнала) или `max_ticks`.
- Профилирование: `RuntimeConfig::profile_modules` — два чтения часов на модуль; результат `module_stats()` (тики, сумма, максимум). Выключено — не платишь ничего.

### 3.5 Сервисы
`bus()`, `jobs()`, `step()`, `tick_arena()` / `previous_tick_arena()` (двойная арена, тег `Engine`), `frame_arena()` (тег `Scratch`), `current_tick()`. `find<M>()` ищет модуль по типу через `dynamic_cast` (первый подходящий), `find(name)` — по имени.

### 3.6 `RuntimeConfig`
`ticks_per_second` (30), `max_ticks_per_frame` (16), `lockstep`, `threads` (-1 = ядра−1, 0 = всё в вызывающем потоке), размеры арен (резерв виртуальный: 256 МиБ ×2 для тика и 64 МиБ для кадра), `profile_modules`. Нулевая/отрицательная частота → `RuntimeError` в конструкторе.

## 4. Как использовать

```cpp
struct Physics final : RuntimeSystem::Module {
    std::string_view name() const noexcept override { return "Physics"; }
    void declare(RuntimeSystem::Runtime& rt) override { rt.bus().declare_module("Physics").produces<Hit>(); }
    void init(RuntimeSystem::Runtime& rt) override { m_out = rt.bus().writer<Hit>(); }
    void tick(RuntimeSystem::Runtime& rt) override { /* ... m_out.emit(...) ... */ }
    EventSystem::EventWriter<Hit> m_out;
};
struct Combat final : RuntimeSystem::Module {
    Combat() { depends_on("Physics"); }
    /* name(), declare(), init(), tick() */
};

RuntimeSystem::Runtime rt({.ticks_per_second = 60.0});
rt.add<Physics>();
rt.add<Combat>();
rt.initialize();
rt.run({.max_ticks = 600, .realtime = false});   // сервер без графики: тики подряд
```

Где это в проекте: **только `Core::App`** (клиент) строится на `Runtime` и пробрасывает `add_module<M>()`; выделенного сервера на нём пока нет — это следующий шаг (см. конспект 16, п. 3.9). Примеры: `examples/01_headless_server.cpp`, `02_lifecycle_and_errors.cpp`; интеграционный тест `Integration.cpu: …runtime_headless`.

## 5. Что менять осторожно

- **Порядок `declare` → `init` → `tick`** и гарантии `shutdown` — контракт для всех модулей. Если сделаешь `init` ленивым или поменяешь порядок, сломаются модули, берущие писателей в `init`.
- `shutdown() noexcept` — не вызывай из него код, который может бросить (иначе `std::terminate`).
- Не добавляй модули после `initialize()`: `m_modules` может перестроиться, а ссылки (`M&` из `add`) — нет, но порядок уже зафиксирован.
- Зависимости — строки: опечатка видна только при `initialize()` как «unknown module». Идея улучшения — `depends_on<Physics>()` (конспект 16, п. 3.6).
- `update()` и `run()` продвигают кадровую шину в разных местах (в `run` — после `begin_frame`); при написании своего цикла повтори эту последовательность: `begin_frame` → `advance_frame` → тики.
- `tick_callback` вызывается **после** всех модулей и **до** `advance_tick`: события, отправленные в нём, видны в следующем тике — как у модулей.
- `find<M>()` использует `dynamic_cast` — не вызывай на горячем пути, храни указатель.

## 6. Упражнения для переписывания

1. Реализуй `depends_on<M>()` по типу и выведи имя из статического `M::module_name`; обнови два модуля в тесте.
2. Сделай `Runtime::add` допустимым после `initialize()` (горячее добавление модуля): что придётся сделать с порядком и `declare`?
3. Добавь параллельные группы: модули без зависимостей друг от друга запускаются через `JobSystem::TaskGroup` — как сохранить детерминированный порядок событий?
4. Напиши выделенный сервер для `CoopScribe` как набор модулей `SimModule` + `NetModule` + `RefereeModule` и сравни с ручным циклом `Player::frame`.
5. Замени `FixedStep` на версию с интерполяцией на целых числах (без `double`) — нужна ли она вне симуляции?

## 7. Тесты

`Modules/RuntimeSystem/tests` (`test_runtime`, `test_modules`, `Probe.hpp`), бенчмарк, 2 примера; `Integration.cpu` проверяет `Core::App` поверх `Runtime`.
