# Sandbox — объединяющий проект FluxEng

Три маленькие симуляции на модулях движка (Core, EventSystem, RendererSystem, WindowSystem).
Цель — увидеть API модулей в настоящем игровом коде и понять, чего им не хватает.

| Цель | Что это | Что показывает |
|---|---|---|
| `engine::Core` (Modules/Core) | слой приложения: окно, рендер, шина, фиксированный тик, пауза/скорость, ввод → события | как модули склеиваются в игровой цикл |
| `FallingSand` | песок, вода, камень, дерево, огонь | массовые SoA-события, два производителя одного события, эффекты с анимацией |
| `Colony` | упрощённый RimWorld: колонисты, ресурсы, склад, хроника | цепочки событий между 6 модулями, атлас, анимации в SoA, граф и циклы |
| `Ecosystem` | трава, зайцы, лисы: популяции колеблются | рождения и смерти, ссылки с поколением (`Handle`), модули-структуры, интерполяция между тиками |

## Запуск

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/bin/FallingSand
build/bin/Colony
build/bin/Ecosystem
build/bin/Colony --frames 600 --screenshot colony.png   # 600 кадров, скриншот, выход
build/bin/Colony --ticks 375                             # ровно 375 тиков: детерминированный прогон
ctest --test-dir build -L sandbox                        # smoke-тесты всех игр
```

На старте каждая игра печатает граф событий и пишет `<Имя>_events.dot`
(`dot -Tsvg Colony_events.dot -o colony.svg`). По F1 и при выходе — граф, порядок модулей,
предупреждения и статистика каналов.

## Управление

Общее: **Space** — пауза, **= / -** — скорость x1…x8, **WASD / стрелки** — камера,
**колесо** — зум к курсору, **F1** — отчёт по шине, **Esc** — выход.
Столбики слева внизу — активность каналов шины в текущем тике (лог-шкала), красная шапка —
были отброшенные по бюджету события. Справа сверху — пауза или скорость.

- **FallingSand:** ЛКМ — рисовать, ПКМ — стирать, **1–5** — материал, **[ / ]** — размер кисти.
- **Colony:** ЛКМ — посадить дерево, ПКМ — положить камень, **J** — линии заданий.
  Полосы слева сверху — брёвна и камень на складе (засечка каждые 10).
- **Ecosystem:** ЛКМ — выпустить 10 зайцев, ПКМ — выпустить 3 лис. Справа внизу — график численности
  (светлая линия — зайцы, оранжевая — лисы). Каждые 300 тиков Census печатает сводку в консоль;
  `stale hunts` в заголовке — охоты, отброшенные проверкой поколения.

## Как написать новую игру

Игра — это `Core::Game` и одна строка в `main()`. Модули игры удобно делать структурами
(как в Ecosystem): свои порты, свои данные, `declare()` с контрактом и `tick()`, куда чужие
данные приходят как `const&`.

```cpp
#include <Core/Core.hpp>

struct Pinged { std::uint32_t id = 0;
    static constexpr std::string_view event_name = "demo.pinged";
    using fields = EventSystem::Fields<EventSystem::Field<"id", &Pinged::id>>; };

struct Pinger {
    EventSystem::EventWriter<Pinged> out;
    void declare(EventSystem::EventBus& bus) { out = bus.writer<Pinged>(bus.declare_module("Pinger").produces<Pinged>()); }
    void tick(EventSystem::Tick now) { out.emit({.id = static_cast<std::uint32_t>(now)}); }
};

class Demo final : public Core::Game {
    Pinger pinger;
    glm::vec2 world_size() const override { return {640, 360}; }
    void setup(Core::App& app) override { pinger.declare(app.bus()); }
    void tick(Core::App& app) override { pinger.tick(app.tick()); }
    void render(Core::App&, RendererSystem::Renderer2D&) override {}
};

int main(int argc, char** argv) { return Core::run<Demo>({.title = "Demo"}, argc, argv); }
```

Добавить цель — одна строка в `foreach(game IN ITEMS ...)` в `Sandbox/CMakeLists.txt` и папка
`<snake_case>/main.cpp`.

## Что нужно дорабатывать (найдено в процессе)

### EventSystem

1. **Нужен второй домен времени.** Ввод доставляется только в тиках симуляции: на паузе
   клики копятся и приходят пачкой после снятия паузы. UI и ввод нужна доставка «на кадр» —
   отдельная шина кадра или политика `Delivery::Frame`.
2. **Нет отложенных событий (`Scheduled`).** Отрастание деревьев в Colony и «молния»
   в FallingSand — ручные очереди по номеру тика.
3. **Нет слияния (`Coalesced`).** FallingSand сам ведёт dirty-флаги, чтобы клетка попадала
   в `cell_changed` не больше раза за тик.
4. **`module_order()` плохо показывает циклы.** Законный цикл World ↔ Jobs ↔ Colonists делает
   «циклическими» и всех, кто ниже (Economy, Chronicle). Нужна конденсация сильно связных
   компонент (Tarjan): цикл — один узел, остальное упорядочено.
5. **Много шаблонного кода.** В Colony 19 полей `EventReader`/`EventWriter`. Нужен объект модуля
   (`ModuleContext`), который получает писателей и читателей по контракту сам.
6. **Ссылки на сущности — голые индексы.** Colony не переиспользует слоты ресурсов, иначе
   запоздалое событие попадёт в новый ресурс. Нужны ID с поколением (задача ECS).

### RendererSystem

1. **Нет текста.** Статус — в заголовке окна, числа — полосками. Следующий шаг — атлас глифов
   на `stb_truetype` (уже есть в ExternalLibrary).
2. **Нельзя обновить текстуру через Renderer2D** (`texture()` константный). Поэтому FallingSand
   рисует до ~27 000 квадов, хотя мог бы обновлять одну текстуру размером с сетку.
   Нужен `Renderer2D::update_texture(handle, image / область)`.
3. **Статичная геометрия пересобирается каждый кадр** (тайлы Colony, клетки FallingSand).
   Для воксельного мира нужны кэшированные буферы чанков.
4. **`advance_animations` принимает только плотный `span<AnimationState>`** — для AoS-данных
   (вспышки в FallingSand) приходится вызывать по одному. Нужна перегрузка с проекцией/шагом.
5. **Нет UV клетки сетки без аллокации** — `make_grid_frames` возвращает `vector`.
   Нужен `SpriteSheetGrid::cell_uv(index)`.
6. **Нет чтения окна в Image** — скриншот в App сделан через `glReadPixels` напрямую.

### WindowSystem

1. **Нет мыши и колеса.** App опрашивает GLFW напрямую, колесо — через `glfwSetScrollCallback`
   со статическим указателем.
2. **Нет `set_title`, размера framebuffer'а, vsync, времени** — всё через GLFW напрямую.
   `getWidth()/getHeight()` до первого resize — размер окна, а не framebuffer'а (HiDPI).
3. **Один слушатель клавиш** (`std::function onKeyPress`), Esc жёстко закрывает окно.
4. **`glfwTerminate` в деструкторе** даёт ложные утечки под LeakSanitizer; счётчик окон
   уменьшается, даже если `gladLoadGLLoader` не сработал.

### Общее

- Модули читают общие данные напрямую (список ресурсов, позиции колонистов). Это место ECS:
  события сообщают *что случилось*, компоненты хранят *как есть сейчас*.
- Ecosystem вручную делает то, что должен дать ECS: `Handle` с поколением, пул слотов (Life)
  и SoA-массивы вида с таблицей «слот → строка» (Herd). Подробности — `!TODO/CONSPECT_2.md`.
