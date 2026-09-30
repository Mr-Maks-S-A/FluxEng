# Конспект 2. Самостоятельные модули, ядро Core и Ecosystem

> Что сделано в ветке `claude/gracious-ritchie-p5qhti` поверх коммита `7d7cd00` (конспект 1).
> Задача была такой: модули движка должны стать самостоятельными библиотеками, слой приложения
> должен переехать из Sandbox в ядро движка, в Sandbox должна появиться третья игра.
> Производительность при этом сохраняется.

---

## Оглавление

1. [Коротко: что сделано](#1-коротко-что-сделано)
2. [Самостоятельные модули](#2-самостоятельные-модули)
3. [Модуль Core — слой приложения](#3-модуль-core--слой-приложения)
4. [Ecosystem — третья игра](#4-ecosystem--третья-игра)
5. [Проверки](#5-проверки)
6. [Что найдено и что дальше](#6-что-найдено-и-что-дальше)
7. [Как продолжить на локальной машине](#7-как-продолжить-на-локальной-машине)

---

## 1. Коротко: что сделано

| Коммит | Что | Главное |
|---|---|---|
| `Модули как самостоятельные библиотеки` | CMake всех модулей, `cmake/FluxModule.cmake`, `tools/check_modules.sh` | `cmake -S Modules/<Name>` собирает и тестирует модуль без корня движка |
| `Core: слой приложения вынесен из Sandbox` | `Sandbox/common` → `Modules/Core`, `FixedStep`, `--ticks N`, `Core::run<G>()` | игра — это `Core::Game` и одна строка в `main()`, прогоны детерминированы |
| `Sandbox: третья игра Ecosystem` | `Sandbox/ecosystem/main.cpp` | рождения и смерти, `Handle` с поколением, модули-структуры |
| `Документация` | `Sandbox/README.md`, этот конспект | как писать новую игру, что найдено |

Не сделано (по договорённости): FallingSand и Colony не переделаны в модули-структуры,
многопоточная шина с локальными буферами не начата.

Цифры (код и CMake, без документации): 30 файлов, +1389 / −257 строк. Тестов было 146, стало 171, все проходят.
Сборка без предупреждений: раньше glm сыпал десятками `-Wsign-conversion`.

---

## 2. Самостоятельные модули

### 2.1. Зачем

Модули и раньше были отдельными статическими библиотеками, но собирались только из корня:
тесты брали `doctest` из `ExternalLibrary`, который подключал корневой `CMakeLists.txt`.
Теперь каждый модуль можно собрать отдельно:

```sh
cmake -S Modules/EventSystem -B build/EventSystem && cmake --build build/EventSystem
ctest --test-dir build/EventSystem
```

Зачем это нужно:
- тесты одного модуля собираются за секунды, без GLFW и рендера;
- модуль нельзя незаметно привязать к чужому коду: самостоятельная сборка упадёт сразу;
- EventSystem в будущем можно выделить в отдельный репозиторий без переделок
  (`git subtree split --prefix=Modules/EventSystem`).

### 2.2. Как устроено: `cmake/FluxModule.cmake`

Каждый модуль в начале своего `CMakeLists.txt` подключает общий файл:

```cmake
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")
```

В нём три функции.

| Функция | Что делает |
|---|---|
| `flux_external(glfw)` | подключает `ExternalLibrary/glfw`, **если цели ещё нет**. Опции библиотеки (выключить примеры, тесты, install) заданы здесь, в одном месте |
| `flux_module(EventSystem)` | подключает соседний модуль, если цели `engine::EventSystem` ещё нет |
| `flux_module_option(VAR "…")` | опция тестов, бенчмарков и примеров: по умолчанию ON только при самостоятельной сборке или при `FLUX_DEVELOPER=ON` |

Корневой `ExternalLibrary/CMakeLists.txt` вызывает те же `flux_external(...)`, поэтому
настройки библиотек одинаковы при любом способе сборки.

**`FLUX_DEVELOPER`** — опция в корне, по умолчанию `${PROJECT_IS_TOP_LEVEL}`:

| Как собирается | Тесты, бенчмарки, примеры модулей |
|---|---|
| `cmake -S .` (FluxEng — главный проект) | собираются |
| `cmake -S Modules/X` (модуль сам по себе) | собираются для этого модуля |
| FluxEng подключён в чужой проект через `add_subdirectory` | не собираются |
| `-DFLUX_DEVELOPER=OFF` | не собираются, только библиотеки и игры |

**SYSTEM.** Сторонние библиотеки подключаются через `add_subdirectory(... SYSTEM)` (CMake 3.25).
Их заголовки становятся системными, и `-Wconversion` модулей перестал ругаться на glm.
Ручные обходы `renderersystem_system_includes()` и `foreach(... SYSTEM ...)` в Sandbox удалены.

### 2.3. Что поменялось в модулях

| Модуль | Изменение |
|---|---|
| EventSystem | опции через `flux_module_option`, `doctest`/`benchmark` через `flux_external` |
| RendererSystem | то же + `glm`/`glad`/`stb`; примеры 01–02 сами подтягивают WindowSystem |
| WindowSystem | убран глобальный `set(CMAKE_CXX_STANDARD 20)` → `target_compile_features(cxx_std_20)` на цели; тесты регистрируются по одному (`WindowSystem.*`) |
| ECSSystem | убрана неиспользуемая зависимость от fmt/spdlog (заголовки её не подключали), модуль **включён** в сборку движка, 8 тестов проходят |
| Core | новый, см. раздел 3 |

Код модулей не менялся: это только CMake.

### 2.4. Зависимости модулей

```
EventSystem      → только std
ECSSystem        → только std
WindowSystem     → GLFW, glad
RendererSystem   → glm, glad, stb           (примеры 01–02 → WindowSystem)
Core             → EventSystem, RendererSystem, WindowSystem, GLFW, stb
Sandbox (игры)   → Core
```

Правило: модуль нижнего уровня никогда не подключает заголовки соседа того же уровня.

### 2.5. Проверка: `tools/check_modules.sh`

```sh
tools/check_modules.sh build-modules -G Ninja     # на Linux без дисплея — через xvfb-run
```

Скрипт собирает и тестирует каждый модуль отдельно и печатает итог. Его стоит добавить в CI:
если модуль незаметно начнёт зависеть от чего-то, что подключает только корень, скрипт упадёт.

```
==== EventSystem      ok (0 tests failed out of 79)
==== WindowSystem     ok (0 tests failed out of 9)
==== ECSSystem        ok (0 tests failed out of 8)
==== RendererSystem   ok (0 tests failed out of 64)
==== Core             ok (0 tests failed out of 8)
```

---

## 3. Модуль Core — слой приложения

### 3.1. Что переехало

| Было | Стало |
|---|---|
| `Sandbox/common/include/Sandbox/App.hpp` | `Modules/Core/src/include/Core/App.hpp` |
| `Sandbox/common/include/Sandbox/PlatformEvents.hpp` | `Modules/Core/src/include/Core/PlatformEvents.hpp` |
| `Sandbox/common/src/App.cpp` | `Modules/Core/src/code/App.cpp` |
| цель `SandboxCore` | `engine::Core` |
| `namespace Sandbox` | `namespace Core` |

Файлы перенесены через `git mv`, история сохранена (`git log --follow`).
Имена событий (`platform.key`, `platform.mouse_button`) не менялись.

### 3.2. Новое в Core

**`Core::run<G>(config, argc, argv)`.** Раньше в каждом `main` был одинаковый `try/catch`:

```cpp
int main(int argc, char** argv) {
    return Core::run<FallingSand>({.title = "FallingSand", .ticks_per_second = 30.0}, argc, argv);
}
```

**`FixedStep`** (`Core/FixedStep.hpp`). Фиксированный тик выделен из `App::run` в отдельный класс:

```cpp
class FixedStep {
    double ticks_per_second; int max_ticks_per_frame; int speed; bool paused; bool lockstep;
    int   advance(double frame_seconds);  // сколько тиков выполнить в этом кадре
    float alpha() const;                  // доля пути к следующему тику, [0, 1)
};
```

Поведение цикла не изменилось: тот же накопитель и та же защита от «спирали смерти».
Класс не зависит от окна, поэтому у него есть юнит-тесты (`Modules/Core/tests`).

**`App::tick_alpha()`** — доля пути к следующему тику. При 30 тиках в секунду и 240 fps позиция
меняется раз в 8 кадров. Если рисовать `mix(prev, pos, alpha)`, движение плавное.
Ecosystem так и делает (`Herd::prev`).

**`--ticks N`** — ровно N тиков, **по одному на кадр**, без учёта реального времени. Результат
не зависит от скорости машины и vsync:

```
FallingSand --ticks 375   →  sand.cell_changed 295570, sand.ignited 797 — одинаково при каждом запуске
Colony      --ticks 375   →  job_assigned 21, resource_harvested 13      — совпадает с конспектом 1
```

Smoke-тесты Sandbox переведены с `--frames 240` на `--ticks 300`: они стали быстрее и детерминированными.

### 3.3. Один кадр (как было, только через FixedStep)

```
poll_input ──▶ for (steps = m_step.advance(dt); steps > 0; --steps) { game.tick(); bus.advance_tick(); }
           ──▶ render (камера мира) ──▶ render_overlay + оверлей шины ──▶ скриншот, заголовок
           ──▶ last_frame? (--frames N или --ticks N) → выход
```

---

## 4. Ecosystem — третья игра

### 4.1. Зачем именно она

FallingSand и Colony почти не создают и не уничтожают сущности: колонистов ровно 10, ресурсы
в Colony не переиспользуют индексы. Ecosystem рождает и убивает сотни существ и
переиспользует слоты. Поэтому она упирается в главный пробел движка — ссылки с поколением.
Это готовый стенд для будущего ECS.

### 4.2. Модули и события

```
Spawner ──┐                           ┌──▶ eco.born ──▶ Rabbits, Foxes, Census
Rabbits ──┼── eco.birth_request ──▶ Life
Foxes ────┤   eco.hunt ───────────▶ Life ──▶ eco.died ──▶ Rabbits, Foxes, Census
Migration ┘   eco.starved ────────▶ Life
Rabbits ── eco.grazed ──▶ Grass
```

| Модуль | Данные | Читает чужое (`const&`) |
|---|---|---|
| Grass | уровень травы по клеткам | — |
| Rabbits | `Herd` зайцев | `Grass` (где трава) |
| Foxes | `Herd` лис | `Rabbits` (где зайцы) |
| Life | пул слотов: `generation[]`, `alive[]`, свободные слоты | — |
| Census | численность и история для графика | `Life` (счётчик stale для печати) |
| Migration | — | `Census` (чтобы вид не вымер) |
| Spawner | — | — (клики приходят событием `platform.mouse_button`) |

Каждый модуль — структура по правилам из обсуждения: порты, свои данные, `declare(bus)`
с контрактом, `tick(...)` без `App&`. Класс игры только вызывает модули по порядку и рисует.

### 4.3. Handle и поколения — главное

```cpp
struct Handle { std::uint32_t index; std::uint32_t generation; };
```

Слоты выдаёт и освобождает **только Life**. При смерти `++generation[index]`, и все старые
ссылки на этот слот перестают быть валидными:

```
тик N     Foxes:  лиса рядом с зайцем {7, gen 3} → emit eco.hunt{fox, rabbit{7,3}}
          (другая лиса в том же тике тоже emit eco.hunt{…, rabbit{7,3}})
тик N+1   Life:   hunt #1 — valid({7,3}) ✔ → слот 7 освобождён, gen 3→4, emit eco.died
                  hunt #2 — valid({7,3}) ✘ (gen уже 4) → stale_hunts++
                  birth_request → слот 7 занят новым зайцем {7, gen 4}
тик N+2   Rabbits: eco.died{7,3} → удалить строку; eco.born{7,4} → добавить новую
          Foxes:   eco.died{cause=eaten, killer=fox} → кормим охотника, если он жив
```

Без поколения охота #2 «съела» бы новорождённого зайца в слоте 7. За 3000 тиков таких
устаревших охот ~500 из ~2000. Это не баг игры, а обычное следствие семантики «тик N → N+1»,
и проверка поколения её честно обрабатывает.

Порядок в Rabbits и Foxes: **сначала смерти, потом рождения**. Слот, освобождённый в прошлом
тике, мог уже достаться новорождённому.

### 4.4. Herd — SoA вида

```cpp
struct Herd {
    std::vector<Handle> handle;  std::vector<glm::vec2> pos, prev;  std::vector<float> heading, energy;
    std::vector<int> cooldown;   std::vector<std::uint8_t> dying;
    std::vector<std::uint32_t> row_of;   // слот Life → строка массивов
    std::size_t find(Handle) const;  void add(...);  void remove(Handle); // swap-remove, массивы плотные
};
```

Это ровно то, что должен давать ECS: компоненты в плотных массивах, сущность = Handle,
таблица «сущность → строка» (sparse set). В ECSSystem уже есть `SparseSet` и `World`:
следующий шаг — переписать Herd на них и сравнить код и замеры.

### 4.5. Баланс

Параметры подбирались серией прогонов `--ticks 4500` (4 варианта параллельно, без отрисовки).
Выбранный вариант даёт классические колебания Лотки–Вольтерры: лисы отстают по фазе,
никто не вымирает.

| Тик | Зайцы | Лисы |
|---|---|---|
| 300 | 308 | 64 |
| 600 | 87 | 82 |
| 900 | 44 | 50 |
| 1200 | 125 | 26 |
| 1500 | 210 | 36 |
| 1800 | 203 | 55 |
| 2100 | 126 | 73 |
| 2400 | 71 | 63 |
| 2700 | 83 | 38 |

Первая версия упиралась в пределы 1500/200 без колебаний: травы было втрое больше, а зайцы
получали от еды почти вдвое больше энергии. Пределы `cap` оставлены как страховка.
Migration приводит особей с края карты, если вид почти исчез (зайцев < 10 или лис < 2).

Параметры — в начале `Sandbox/ecosystem/main.cpp` (`rabbit_rules`, `fox_rules`, `fox_sight`,
`grass_regrow_per_tick`).

### 4.6. Цифры шины за 3000 тиков

| Канал | Событий | Пик за тик |
|---|---|---|
| `eco.grazed` | 202 681 | 161 |
| `eco.birth_request` | 2 284 | 189 (стартовая популяция) |
| `eco.born` | 2 283 | 189 |
| `eco.died` | 2 077 | 5 |
| `eco.hunt` | 2 010 | 10 |
| `eco.starved` | 618 | 5 |

---

## 5. Проверки

| Проверка | Результат |
|---|---|
| Сборка всего движка (GCC 14, RelWithDebInfo) | без ошибок и предупреждений |
| `ctest` (под Xvfb, программный OpenGL Mesa) | 171/171 (было 146) |
| Каждый модуль отдельно (`tools/check_modules.sh`) | 5/5 модулей |
| `-DFLUX_DEVELOPER=OFF` | собираются только библиотеки и игры |
| Детерминизм `--ticks` | два прогона дают одинаковую статистику каналов (FallingSand, Colony, Ecosystem) |
| Пример «новой игры» из `Sandbox/README.md` | компилируется и запускается |
| Бенчмарки до/после | в пределах шума, см. ниже |

**Бенчмарки.** Флаги компиляции кода модулей идентичны: изменилось только `-I` → `-isystem`
у заголовков google benchmark. Медианы из 7 повторов, запуски чередовались (нс):

| Замер | до | после | до (2) | после (2) |
|---|---|---|---|---|
| `ReadOneField_SoA/32768` | 42 457 | 42 105 | 41 133 | 41 431 |
| `AdvanceTick/512` | 13 247 | 15 569 | 15 842 | 13 619 |
| `AcquireWriter` | 29.3 | 32.6 | 36.6 | 31.4 |
| `SpriteBatch 16 textures sorted/32768` | 1 477 561 | 1 398 462 | — | — |

Разброс между соседними запусками одного и того же бинарника больше, чем между «до» и «после».
Это шум общего контейнера, а не изменение производительности.

**Где не проверено:** окна Sandbox на настоящей видеокарте (только Xvfb + Mesa llvmpipe),
Windows и MSVC, Clang. Это стоит прогнать локально.

---

## 6. Что найдено и что дальше

### Найдено по ходу

1. **`module_order()` и циклы** (пункт 4 конспекта 1) снова видны: в Ecosystem законный цикл
   Life ↔ Rabbits/Foxes метит как «циклических» Grass, Census и всех ниже. Нужен Тарьян.
2. **Scheduled.** Migration ведёт паузу `next_allowed` вручную. Census считает с задержкой
   в пару тиков, поэтому без паузы Migration заказывала бы переселенцев несколько тиков подряд.
3. **Заголовок WindowSystem не чист под `-Wshadow`/`-Wpedantic`**, поэтому Core подключает его
   как SYSTEM. Лучше исправить сам `Window.hpp`.
4. **`.gitmodules` на SSH** (`git@github.com:`). CI и облачные сессии без ключа не склонируют подмодули.
   Обход: `git -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init`.
   Для CI лучше перевести URL на `https://`.
5. **GLFW и Wayland.** Без `wayland-scanner` конфигурация падает; в контейнерах и CI —
   `-DGLFW_BUILD_WAYLAND=OFF`. На Arch с Wayland всё на месте.
6. **Детерминизм — только в пределах одной стандартной библиотеки.**
   `std::uniform_*_distribution` дают разные числа в libstdc++, libc++ и MSVC.
   Для сети и повторов между платформами нужен свой генератор и свои распределения.
7. **Ecosystem вышла на 769 строк** вместо оценки 350–400: половина — модули-структуры и
   комментарии к ним, плюс Herd (~100 строк), который потом заменит ECS.

### Что дальше (по приоритету)

| # | Задача | Зачем |
|---|---|---|
| 1 | Herd → ECSSystem (`SparseSet`, `World`, Handle из ECS) | первая настоящая проверка ECS на живом коде, сравнить строки и замеры |
| 2 | FallingSand и Colony → модули-структуры | ТЗ из обсуждения, часть 1; Ecosystem — образец |
| 3 | WindowSystem: мышь, колесо, `set_title`, framebuffer, время | убрать прямые вызовы GLFW из Core |
| 4 | EventSystem: Тарьян в `module_order()` | Ecosystem и Colony показывают проблему |
| 5 | CI: `CMakePresets.json` + `tools/check_modules.sh` + `ctest` | матрица компиляторов из обсуждения |
| 6 | Локальные буферы писателей | многопоточная шина |

---

## 7. Как продолжить на локальной машине

```sh
git fetch origin claude/gracious-ritchie-p5qhti
git checkout claude/gracious-ritchie-p5qhti
git submodule update --init

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build                       # 171 тест; GPU-тесты требуют дисплей

build/bin/Ecosystem                          # ЛКМ — зайцы, ПКМ — лисы
build/bin/Ecosystem --ticks 3000             # детерминированный прогон со сводкой Census
tools/check_modules.sh build-modules -G Ninja
```

CMake теперь требует версию **3.25+** (ради `add_subdirectory(... SYSTEM)` и `PROJECT_IS_TOP_LEVEL`).
