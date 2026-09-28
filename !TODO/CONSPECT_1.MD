# Конспект 1. Sandbox: как EventSystem, RendererSystem и WindowSystem работают вместе

> Предмет конспекта — папка `FluxEng/Sandbox`: каркас `SandboxCore` и две игры, `FallingSand` и `Colony`.
> Главный вопрос: **как применены разработанные системы**. Поэтому каждое место разбирается с кодом,
> схемой взаимодействия и тем, что из него следует для движка.

---

## Оглавление

1. [Зачем нужен Sandbox и что в нём лежит](#1-зачем-нужен-sandbox-и-что-в-нём-лежит)
2. [Общая архитектура](#2-общая-архитектура)
3. [SandboxCore — каркас приложения](#3-sandboxcore--каркас-приложения)
4. [EventSystem на практике — общие приёмы](#4-eventsystem-на-практике--общие-приёмы)
5. [FallingSand — клеточная симуляция](#5-fallingsand--клеточная-симуляция)
6. [Colony — упрощённый RimWorld](#6-colony--упрощённый-rimworld)
7. [RendererSystem на практике](#7-renderersystem-на-практике)
8. [Замеры производительности](#8-замеры-производительности)
9. [Что нужно дорабатывать и почему](#9-что-нужно-дорабатывать-и-почему)
10. [Шпаргалка по API](#10-шпаргалка-по-api)

---

## 1. Зачем нужен Sandbox и что в нём лежит

Модули движка разрабатывались по отдельности и проверялись тестами. Sandbox — первое место,
где они работают **вместе в настоящем игровом цикле**. Его задачи:

- увидеть, как API выглядит в прикладном коде: удобно ли, много ли шаблонного кода;
- найти, чего не хватает (найденное записано в раздел 9);
- получить эталонные примеры, которые можно запускать и показывать.

```
Sandbox/
├── CMakeLists.txt              сборка: SandboxCore + FallingSand + Colony + smoke-тесты
├── README.md                   запуск, управление, список доработок
├── common/                     SandboxCore — общий каркас
│   ├── include/Sandbox/
│   │   ├── App.hpp             App, Game, AppConfig, FrameInput          (136 строк)
│   │   └── PlatformEvents.hpp  KeyEvent, MouseButtonEvent                ( 40 строк)
│   └── src/App.cpp             цикл кадров и тиков, ввод, оверлей, отчёт (295 строк)
├── falling_sand/main.cpp       игра 1: песок, вода, огонь                (470 строк)
└── colony/main.cpp             игра 2: колонисты, ресурсы, склад         (647 строк)
```

Сборка и запуск:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/bin/FallingSand
build/bin/Colony --frames 600 --screenshot colony.png   # 600 кадров, скриншот, выход
ctest --test-dir build -L sandbox                        # smoke-тесты обеих игр
```

`CMakeLists.txt` собирает каркас как статическую библиотеку, а игры подключают её одной строкой:

```cmake
add_library(SandboxCore STATIC common/src/App.cpp)
target_link_libraries(SandboxCore PUBLIC
    engine::EventSystem
    engine::RendererSystem
    engine::WindowSystem
    glfw
    stb
)

foreach(game IN ITEMS FallingSand Colony)
    # FallingSand -> falling_sand
    string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" folder "${game}")
    string(TOLOWER "${folder}" folder)
    add_executable(${game} ${folder}/main.cpp)
    target_link_libraries(${game} PRIVATE SandboxCore)
    add_test(NAME Sandbox.${game} COMMAND ${game} --frames 240)   # smoke-тест с GPU
endforeach()
```

**Нюанс сборки.** В прикладном коде включены `-Wall -Wextra -Wpedantic -Wshadow`, но не `-Wconversion`:
игровой код полон преобразований `int ↔ float`, и `static_cast` на каждом шагу хуже читается.
В модулях движка `-Wconversion` включён. Заголовки GLFW, glm, glad, stb и WindowSystem подключены
как системные (`SYSTEM`), чтобы их предупреждения не смешивались с нашими.

---

## 2. Общая архитектура

```
┌──────────────────────────────── процесс игры ────────────────────────────────┐
│                                                                              │
│  main()                                                                      │
│    └── Sandbox::App app(config);   FallingSand game;   app.run(game);        │
│                                                                              │
│  ┌──────────────────────── SandboxCore (App) ─────────────────────────┐      │
│  │                                                                    │      │
│  │   Window (WindowSystem) ── GL-контекст, клавиатура (onKeyPress)    │      │
│  │        │                                                           │      │
│  │        ▼  колбэки / опрос GLFW                                     │      │
│  │   модуль "Platform" ──emit──▶ platform.key, platform.mouse_button  │      │
│  │                                        │                           │      │
│  │   EventSystem::EventBus ◀──────────────┘   (каналы всех событий)   │      │
│  │        ▲         │ advance_tick() — после каждого тика             │      │
│  │        │         ▼                                                 │      │
│  │   ┌──────── Game (FallingSand / Colony) ─────────┐                 │      │
│  │   │ setup(): модули, контракты, writer/reader    │                 │      │
│  │   │ tick():  модуль за модулем                   │                 │      │
│  │   │ render(): спрайты мира                       │─────┐           │      │
│  │   └──────────────────────────────────────────────┘     │           │      │
│  │                                                        ▼           │      │
│  │   RendererSystem::Renderer2D ── SpriteBatch ── OpenGL 3.3          │      │
│  │   Camera2D (мир)  +  экранная камера (оверлей)                     │      │
│  └────────────────────────────────────────────────────────────────────┘      │
└──────────────────────────────────────────────────────────────────────────────┘
```

Три правила, на которых всё построено:

1. **Модули игры общаются через события.** Прямых вызовов «модуль → модуль» нет.
2. **Каждый модуль объявляет контракт**: что он порождает и что читает. Шина проверяет контракт
   при выдаче писателей и читателей и строит по нему граф.
3. **Рендер не знает о логике.** Игра в `render()` превращает своё состояние в `SpriteInstance`,
   а `Renderer2D` превращает их в минимум draw call'ов.

---

## 3. SandboxCore — каркас приложения

### 3.1. Интерфейс игры

Игра — это класс с четырьмя обязательными методами. Всё остальное делает `App`.

```cpp
class Game {
public:
    virtual ~Game() = default;
    virtual glm::vec2 world_size() const = 0;               // для начальной камеры
    virtual void setup(App& app) = 0;                        // модули, контракты, текстуры
    virtual void tick(App& app) = 0;                         // один тик симуляции
    virtual void render(App& app, RendererSystem::Renderer2D& r) = 0;       // мир
    virtual void render_overlay(App&, RendererSystem::Renderer2D&) {}      // экран, опционально
    virtual std::string status() const { return {}; }        // в заголовок окна
};
```

`main` у обеих игр одинаковый:

```cpp
int main(int argc, char** argv) {
    try {
        Sandbox::AppConfig config;
        config.title = "FallingSand";
        config.ticks_per_second = 30.0;
        Sandbox::App app(Sandbox::parse_args(std::move(config), argc, argv)); // --frames, --screenshot
        FallingSand game;
        return app.run(game);
    } catch (const std::exception& error) {
        std::println(stderr, "FallingSand: {}", error.what());
        return 1;
    }
}
```

> **Нюанс.** Конфиг заполняется присваиваниями, а не `{.title = ..., .ticks_per_second = ...}`:
> при designated-инициализации GCC предупреждает о пропущенном поле `screenshot` (`std::string`).

### 3.2. Порядок владения в `App`

```cpp
class App {
    AppConfig m_config;
    Window m_window;                                       // 1. окно и GL-контекст
    std::optional<RendererSystem::Renderer2D> m_renderer;  // 2. рендер — только после контекста
    EventSystem::EventBus m_bus;
    RendererSystem::Camera2D m_camera;
    ...
};
```

- **Порядок членов важен.** C++ разрушает члены в обратном порядке, поэтому рендер
  (он удаляет GL-буферы и текстуры) уничтожается **до** окна, пока GL-контекст ещё жив.
- `Renderer2D` создаётся фабрикой `Renderer2D::create()` → `std::expected`, конструктора по
  умолчанию у него нет. Поэтому член — `std::optional`, а создаётся он в теле конструктора `App`,
  когда окно уже есть:

```cpp
App::App(AppConfig config)
    : m_config(std::move(config)), m_window(m_config.width, m_config.height, m_config.title, true) {
    if (m_window.getNativeWindow() == nullptr) {
        throw std::runtime_error("cannot create window / OpenGL context");
    }
    auto renderer = Renderer2D::create();
    if (!renderer) {
        throw std::runtime_error(renderer.error());   // текст ошибки компиляции шейдера и т.п.
    }
    m_renderer.emplace(std::move(*renderer));
    ...
}
```

### 3.3. Два домена времени и фиксированный тик

Это центральная идея каркаса. У игры в духе RimWorld есть пауза и скорость x1…x8,
поэтому время симуляции нельзя привязывать к кадрам.

```
реальное время ──────────────────────────────────────────────────────────▶

кадры (vsync 240 Гц):  │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │
                       ввод, камера, отрисовка — каждый кадр, даже на паузе

тики x1 (30/с):        ┃       ┃       ┃       ┃       ┃       ┃       ┃
тики x4 (120/с):       ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃ ┃
пауза:                 (тиков нет, bus.advance_tick() не вызывается)

на каждом ┃:  game.tick(app);  bus.advance_tick();
```

Реализация — классический «аккумулятор времени»:

```cpp
const double tick_dt = 1.0 / m_config.ticks_per_second;
constexpr int max_ticks_per_frame = 16; // защита от «спирали смерти» на медленном кадре

for (int frame = 0; !m_window.shouldClose() && frame != m_config.max_frames; ++frame) {
    const double now = glfwGetTime();
    const double frame_dt = std::min(now - last, 0.25);   // после отладчика/свёрнутого окна
    last = now;
    ...
    if (!m_paused) {
        m_tick_accumulator += frame_dt * m_speed;          // скорость игры — множитель времени
        int steps = 0;
        while (m_tick_accumulator >= tick_dt && steps < max_ticks_per_frame) {
            game.tick(*this);
            m_bus.advance_tick();                          // события тика N станут видны в N+1
            m_tick_accumulator -= tick_dt;
            ++steps;
        }
        if (steps == max_ticks_per_frame) {
            m_tick_accumulator = 0.0;                      // не пытаемся «догнать» бесконечно
        }
    }
    // ... отрисовка ...
}
```

Разбор:

| Деталь | Зачем |
|---|---|
| `tick_dt` постоянный | симуляция детерминирована: тот же ввод → тот же результат при любом FPS |
| `frame_dt * m_speed` | x4 — это просто в 4 раза больше тиков за кадр, логика игры про скорость не знает |
| `std::min(..., 0.25)` | после паузы в отладчике не будет рывка в сотни тиков |
| `max_ticks_per_frame = 16` | если тик дороже кадра, игра замедляется, а не зависает («спираль смерти») |
| `advance_tick()` после каждого тика | тик шины = тик симуляции (решение из дизайна EventSystem) |

**Время внутри игры** берётся из `App`, а не из часов:

```cpp
float tick_seconds() const { return static_cast<float>(1.0 / m_config.ticks_per_second); }
EventSystem::Tick tick() const { return m_bus.current_tick(); }
```

Анимации продвигаются на `tick_seconds()`, поэтому при x4 колонисты и машут руками в 4 раза быстрее,
а на паузе замирают.

### 3.4. Порядок работы в одном кадре

```
┌─ кадр ───────────────────────────────────────────────────────────────────────────┐
│ 1. frame_dt, размер framebuffer'а → camera.viewport                               │
│ 2. poll_input():  WASD-камера, мышь → мир, колесо → зум, кнопки → MouseButtonEvent│
│ 3. если не пауза:  0..16 × { game.tick(); bus.advance_tick(); }                   │
│ 4. renderer.clear();  begin(camera мира);   game.render();          end();        │
│ 5.                     begin(камера экрана); game.render_overlay();                │
│                                              draw_bus_overlay();    end();        │
│ 6. [--screenshot на последнем кадре] glReadPixels(GL_BACK)                        │
│ 7. window.update()  →  glfwSwapBuffers + glfwPollEvents  (здесь срабатывает       │
│                        onKeyPress → KeyEvent в шину)                              │
└───────────────────────────────────────────────────────────────────────────────────┘
```

Два `begin/end` за кадр — нормальная практика: мир рисуется камерой мира, панели — камерой,
у которой одна единица мира равна одному пикселю экрана:

```cpp
r.begin(Camera2D{.position = m_camera.viewport * 0.5f, .viewport = m_camera.viewport});
```

Центр такой камеры стоит в середине экрана, поэтому мировые координаты `(0, 0)` — это левый
верхний пиксель, а `(width, height)` — правый нижний.

### 3.5. Ввод → события: модуль "Platform"

Окно (WindowSystem) даёт колбэк клавиш, но ничего не знает о шине. `App` — это **мост**:
он объявляет модуль "Platform" и переводит ввод в события.

```cpp
// PlatformEvents.hpp
struct KeyEvent {
    std::int32_t key = 0;    // GLFW_KEY_*
    std::int32_t action = 0; // GLFW_PRESS / GLFW_RELEASE / GLFW_REPEAT

    static constexpr std::string_view event_name = "platform.key";
    using fields = EventSystem::Fields<
        EventSystem::Field<"key", &KeyEvent::key>,
        EventSystem::Field<"action", &KeyEvent::action>>;
};

struct MouseButtonEvent {
    std::int32_t button = 0;
    std::int32_t action = 0;
    float world_x = 0.0f;    // координаты уже переведены камерой в мир
    float world_y = 0.0f;
    ...
};
```

```cpp
// App::App — контракт модуля платформы
m_platform = m_bus.declare_module("Platform")
                 .produces<KeyEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 1024})
                 .produces<MouseButtonEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 1024});
m_key_out = m_bus.writer<KeyEvent>(m_platform);        // писатель с проверкой контракта
m_mouse_out = m_bus.writer<MouseButtonEvent>(m_platform);

m_window.onKeyPress = [this](int key, int action) { on_key(key, action); };
```

```cpp
void App::on_key(int key, int action) {
    m_key_out.emit(KeyEvent{.key = key, .action = action});   // 1) всё — в шину для игры
    if (action != GLFW_PRESS) return;
    switch (key) {                                            // 2) клавиши самого каркаса
        case GLFW_KEY_SPACE: m_paused = !m_paused; break;
        case GLFW_KEY_EQUAL:
        case GLFW_KEY_KP_ADD: m_speed = std::min(m_speed * 2, 8); break;
        case GLFW_KEY_MINUS:
        case GLFW_KEY_KP_SUBTRACT: m_speed = std::max(m_speed / 2, 1); break;
        case GLFW_KEY_F1: print_event_report(); break;
        default: break;
    }
}
```

Мышь в WindowSystem не пробрасывается, поэтому она **опрашивается** каждый кадр,
а событие создаётся на **фронте** (кнопка сменила состояние):

```cpp
constexpr int buttons[3] = {GLFW_MOUSE_BUTTON_LEFT, GLFW_MOUSE_BUTTON_RIGHT, GLFW_MOUSE_BUTTON_MIDDLE};
for (std::size_t i = 0; i < 3; ++i) {
    const bool down = glfwGetMouseButton(native, buttons[i]) == GLFW_PRESS;
    m_input.pressed[i] = down && !m_input.down[i];            // нажата именно в этом кадре
    if (down != m_input.down[i]) {                            // фронт: press или release
        m_mouse_out.emit(MouseButtonEvent{.button = buttons[i], .action = down ? GLFW_PRESS : GLFW_RELEASE,
                                          .world_x = m_input.mouse_world.x, .world_y = m_input.mouse_world.y});
    }
    m_input.down[i] = down;
}
```

Итог: у игры есть **два способа** получить ввод, и игры используют оба.

| Способ | Что это | Кто использует |
|---|---|---|
| событие `platform.key` / `platform.mouse_button` | дискретные нажатия, приходят в тик N+1 | Brush (выбор материала), Orders (клики) |
| `app.input()` → `FrameInput` | состояние «прямо сейчас»: позиция мыши, удержание | Brush (рисование при зажатой кнопке) |

> **Нюанс: события ввода живут в домене тиков.** На паузе тиков нет, поэтому клики копятся
> в `pending` и приходят пачкой после снятия паузы. Для игры это терпимо, для UI — нет.
> Это одна из главных находок (раздел 9).

### 3.6. Мышь, HiDPI и зум к курсору

```cpp
glfwGetCursorPos(native, &mx, &my);
glfwGetWindowSize(native, &window_w, &window_h);
// Курсор приходит в координатах окна, камера — в пикселях framebuffer'а (HiDPI).
const glm::vec2 scale = window_w > 0 ? m_camera.viewport / glm::vec2(window_w, window_h) : glm::vec2(1.0f);
m_input.mouse_screen = glm::vec2(mx, my) * scale;

// Зум к курсору: точка мира под курсором остаётся на месте.
if (m_pending_scroll != 0.0f) {
    const glm::vec2 before = m_camera.screen_to_world(m_input.mouse_screen);
    m_camera.zoom = std::clamp(m_camera.zoom * std::pow(1.15f, m_pending_scroll), 0.1f, 20.0f);
    m_camera.position += before - m_camera.screen_to_world(m_input.mouse_screen);
    m_pending_scroll = 0.0f;
}
m_input.mouse_world = m_camera.screen_to_world(m_input.mouse_screen);
```

- **HiDPI.** На мониторах с масштабированием окно 1280×720 может иметь framebuffer 2560×1440.
  Курсор GLFW отдаёт в координатах окна, а камера работает в пикселях framebuffer'а, поэтому нужен `scale`.
- **Зум к курсору** — трюк из `Camera2D`: запомнили точку мира под курсором, поменяли зум,
  сдвинули камеру на разницу. Работает и с поворотом камеры, потому что `screen_to_world` учитывает поворот.
- **Колесо** приходит только колбэком. WindowSystem его не пробрасывает, поэтому используется
  `glfwSetScrollCallback` со статическим указателем `App::s_active`: пользовательский указатель
  GLFW-окна уже занят объектом `Window`.

```cpp
s_active = this;
glfwSetScrollCallback(m_window.getNativeWindow(), [](GLFWwindow*, double, double y) {
    if (s_active != nullptr) s_active->m_pending_scroll += static_cast<float>(y);
});
```

### 3.7. Оверлей шины — отладка без текста

Текста в рендере нет, поэтому активность шины показана столбиками: по столбику на канал.

```cpp
for (std::size_t i = 0; i < m_bus.channel_count(); ++i) {
    const es::ChannelStats stats = m_bus.channel_at(i).stats();
    const float height = 3.0f + 10.0f * std::log2(1.0f + static_cast<float>(stats.readable));
    const float x = 10.0f + static_cast<float>(i) * 14.0f;
    r.fill_rect({{x, base - height}, {10.0f, height}}, palette[i % std::size(palette)], 1);
    if (stats.total_dropped > 0) {
        r.fill_rect({{x, base - 88.0f}, {10.0f, 4.0f}}, Colors::red, 2);   // были потери по бюджету
    }
}
```

```
  ▲ высота = log2(1 + событий в тике)
  │           ██                         █ — sand.cell_changed (~3000/тик)
  │           ██                         ▄ — sand.paint (1–3/тик)
  │           ██                         ▁ — пустые каналы
  │     ▄     ██  ▁
  │ ▁ ▁ █  ▁  ██  █
  └──────────────────▶ каналы в порядке регистрации
```

- **Логарифмическая шкала** нужна потому, что каналы отличаются на три порядка (1 событие против 3000).
- Данные берутся через `bus.channel_at(i).stats()` — это **нетипизированный** доступ к шине.
  Каркас не знает ни одного события игры, но показывает их все: для этого `IChannel` и нужен.

Строка состояния уходит в заголовок окна раз в полсекунды:
`Colony | tick 1234 | x2 | 240 fps | 3 draw calls | logs 12 | stone 5 | ...`

### 3.8. Отчёт по шине

При старте, по F1 и при выходе `App` печатает граф, порядок модулей, предупреждения и статистику:

```cpp
const es::EventGraph graph = m_bus.build_graph();
std::println("{}", graph.to_text());                       // дерево модуль → события → потребители
const es::ModuleOrder order = graph.module_order();         // топологический порядок и циклы
for (const es::EventId id : graph.unconsumed_events()) {    // кто-то пишет, никто не читает
    std::println("warning: nobody consumes '{}'", graph.find_event(id)->name);
}
...
std::ofstream(dot_name) << graph.to_dot();                  // FallingSand_events.dot для Graphviz
```

Реальный вывод FallingSand после 3000 кадров:

```
module order: Platform Brush Weather Simulation View Effects
warning: nobody consumes 'platform.mouse_button'
channel                           emitted  peak/tick    dropped       memory
platform.key                            0          0          0          1 KB
platform.mouse_button                   0          0          0          2 KB
sand.paint                            563          3          0          8 KB
sand.cell_changed                  294661       3240          0        639 KB
sand.ignited                          796         13          0          1 KB
```

Предупреждение про `platform.mouse_button` — **честное**: Brush читает мышь опросом
(`app.input()`), а не событием. Граф ловит именно такие вещи.

### 3.9. Скриншот

```cpp
void App::save_screenshot(int width, int height) const {
    // В RendererSystem нет чтения окна (только Framebuffer::read_pixels), поэтому glReadPixels напрямую.
    Image image(width, height);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);                         // кадр уже нарисован, но ещё не показан
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels().data());
    image.flip_vertically();                       // в OpenGL строка 0 — низ, в Image — верх
    stbi_write_png(m_config.screenshot.c_str(), width, height, 4, image.pixels().data(), width * 4);
}
```

- Читается **задний** буфер **до** `swapBuffers`: после обмена его содержимое не определено.
- `Image` из RendererSystem используется как контейнер пикселей, `flip_vertically()` — оттуда же.

---

## 4. EventSystem на практике — общие приёмы

### 4.1. Как объявляется событие

Все события обеих игр устроены одинаково:

```cpp
struct IgnitedEvent {
    std::int32_t x = 0;
    std::int32_t y = 0;

    static constexpr std::string_view event_name = "sand.ignited";            // → EventId (FNV-1a)
    using fields = es::Fields<es::Field<"x", &IgnitedEvent::x>,                // все члены структуры
                              es::Field<"y", &IgnitedEvent::y>>;
};
```

Правила, которые проверяет шина:

| Правило | Где проверяется | Что будет при нарушении |
|---|---|---|
| trivially copyable (никаких `std::string`) | концепт `Event` | ошибка компиляции |
| поля принадлежат этой структуре | концепт `Event` | ошибка компиляции |
| в `fields` перечислены **все** члены | `validate_schema()` при регистрации | `EventSystemError` |
| имя уникально для схемы | `register_event` | `EventSystemError` |

**Нюанс: без неявного padding'а.** Поля подбираются так, чтобы структура была плотной:

```cpp
struct CellChangedEvent {
    std::int32_t x = 0;         // 4
    std::int32_t y = 0;         // 4
    std::uint16_t material = 0; // 2
    std::uint16_t life = 0;     // 2  → итого 12 байт, дыр нет
    ...
};
```

**Урок, пойманный по ходу.** В первой версии `life` был упакован в старшие биты `material`
(`material | life << 8`). Это ровно тот «магический» код, от которого избавляет типизированная схема:
читатель должен знать про сдвиг. Правильно — отдельное поле. Схема сама по себе документирует формат.

### 4.2. Контракт модуля

Модуль — это **имя + список событий**. Объявление одновременно регистрирует каналы:

```cpp
const es::ModuleId simulation =
    bus.declare_module("Simulation")
        .consumes<PaintEvent>()
        .produces<CellChangedEvent>(es::ChannelConfig{.reserve = grid_w * grid_h,
                                                      .max_events_per_tick = grid_w * grid_h})
        .produces<IgnitedEvent>(es::ChannelConfig{.reserve = 64, .max_events_per_tick = 256});
```

- `ChannelConfig::reserve` — память заранее. `cell_changed` может получить до одного события
  на клетку, поэтому резерв = размер сетки: после прогрева аллокаций нет.
- `ChannelConfig::max_events_per_tick` — бюджет. Для `ignited` 256: цепная реакция огня
  не может завалить тик событиями, лишние отбрасываются и видны в статистике (`dropped`).
- `declare_module(...)` возвращает `ModuleBuilder`, который неявно превращается в `ModuleId`.

Писатели и читатели получаются **один раз** в `setup()` и хранятся в членах класса.
Поиск канала и сверка схемы происходят только здесь, а не на каждом `emit`:

```cpp
m_sim_paint   = bus.reader<PaintEvent>(simulation);        // проверка: Simulation объявил consumes
m_sim_changes = bus.writer<CellChangedEvent>(simulation);  // проверка: Simulation объявил produces
```

Если модуль попросит писателя события, которое не объявлял, `setup()` бросит исключение:
`module 'View' did not declare that it produces 'sand.cell_changed'`. Незадокументированная
зависимость ловится на старте, а не через месяц.

### 4.3. Семантика «тик N → тик N+1»

Всё, что модуль отправил в тике N, **все** читатели увидят в тике N+1, ровно один тик.

```
                тик N                        advance_tick()               тик N+1
   ┌─────────────────────────────┐                             ┌─────────────────────────────┐
   │ Jobs:  emit(JobAssigned)    │──▶ pending ─── swap ──▶ ready │ Colonists: events() → видит │
   │ Colonists: events() → пусто │                             │ Jobs:  emit(...) → pending  │
   └─────────────────────────────┘                             └─────────────────────────────┘
```

Следствия, которые видны в играх:

- **Порядок модулей внутри тика не влияет на корректность.** В `Colony::tick()` модули вызываются
  по порядку, но если переставить строки, логика не сломается: каждый читает прошлый тик.
- **Задержка цепочки = длина цепочки в тиках.** Клик в Colony доходит до хроники за 3 тика
  (разобрано в 6.4). При 30 тиках/с это 0.1 с — незаметно.
- **Цикл не зависает.** World → Colonists → World — законный цикл, ответ приходит на следующем тике.

### 4.4. Чтение: AoS или SoA

Способ чтения зависит от раскладки, и ошибка ловится компилятором:

```cpp
// AoS-событие (по умолчанию): читаем структуры целиком
for (const PaintEvent& paint : m_sim_paint.events()) { ... paint.x, paint.material ... }

// SoA-событие (layout = SoA): читаем колонки — только нужные поля
const auto xs = m_view_changes.column<&CellChangedEvent::x>();          // span<const int32_t>
const auto materials = m_view_changes.column<&CellChangedEvent::material>();
```

`m_view_changes.events()` для SoA-события не скомпилируется (`static_assert`), как и
`column<&PaintEvent::x>()` для AoS.

**Когда выбирать SoA.** Только `cell_changed`: тысячи событий за тик, читатель проходит по ним
плотным циклом. Остальные события — единицы за тик, для них AoS проще и не медленнее.

---

## 5. FallingSand — клеточная симуляция

### 5.1. Что это

Сетка 220×124 клеток по 6 пикселей. Материалы: песок, вода, камень, дерево, огонь.
Сверху непрерывно сыплются песок и вода, раз в 8 секунд «молния» поджигает деревянный блок.
Игрок рисует мышью.

Что демонстрирует:
- **массовые SoA-события** — `sand.cell_changed`, до 3240 за тик;
- **два производителя одного события** — кисть игрока и «погода» пишут `sand.paint`;
- **зеркало мира из событий** — View не читает сетку симуляции, только события;
- **эффекты с анимацией** — вспышки на `sand.ignited`.

### 5.2. Граф модулей

```
                  platform.key
   ┌──────────┐ ─────────────────▶ ┌────────┐
   │ Platform │                    │ Brush  │──┐
   └──────────┘ platform.mouse_btn └────────┘  │ sand.paint
        │         (никто не читает)             ├──────────────▶ ┌────────────┐
        ▼                            ┌─────────┐│                │ Simulation │
   ⚠ warning в отчёте                │ Weather │┘                └─────┬──────┘
                                     └─────────┘                       │
                                          sand.cell_changed (SoA, ~3000/тик)
                                     ┌─────────────────────────────────┤
                                     ▼                                 │ sand.ignited
                                 ┌──────┐                              ▼
                                 │ View │                         ┌─────────┐
                                 └──────┘                         │ Effects │
                                                                  └─────────┘

   module order: Platform → Brush → Weather → Simulation → View → Effects   (циклов нет)
```

### 5.3. События

```cpp
enum class Material : std::uint8_t { Empty, Sand, Water, Stone, Wood, Fire };

struct PaintEvent {                 // «закрасить круг клеток» — AoS
    std::int32_t x = 0, y = 0;
    std::uint32_t material = 0;     // Material, расширенный до 4 байт (без padding'а)
    std::uint32_t radius = 0;
    static constexpr std::string_view event_name = "sand.paint";
    ...
};

struct CellChangedEvent {           // «клетка сменила материал» — SoA
    std::int32_t x = 0, y = 0;
    std::uint16_t material = 0;
    std::uint16_t life = 0;
    static constexpr std::string_view event_name = "sand.cell_changed";
    static constexpr es::Layout layout = es::Layout::SoA;
    ...
};

struct IgnitedEvent {               // «дерево загорелось» — AoS
    std::int32_t x = 0, y = 0;
    static constexpr std::string_view event_name = "sand.ignited";
    ...
};
```

### 5.4. Один тик

```cpp
void tick(Sandbox::App& app) override {
    tick_brush(app);      // ввод игрока      → sand.paint
    tick_weather(app);    // демо-активность  → sand.paint
    tick_simulation();    // sand.paint (прошлый тик) → физика → sand.cell_changed, sand.ignited
    tick_view();          // sand.cell_changed (прошлый тик) → зеркало мира
    tick_effects(app);    // sand.ignited (прошлый тик) → вспышки
}
```

Все модули живут в одном классе, но **данные у каждого свои**, а общаются они только событиями:

```cpp
// ---- Brush
Material m_brush = Material::Sand;
int m_radius = 3;
// ---- Simulation
std::vector<Material> m_cells;           // сетка
std::vector<std::uint8_t> m_life;        // время горения
std::vector<std::uint32_t> m_moved;      // «уже двигалась в этом тике»
std::vector<std::uint8_t> m_dirty;       // «изменилась в этом тике»
std::vector<std::uint32_t> m_dirty_list;
// ---- View
std::vector<Material> m_view;            // своё зеркало мира!
std::vector<std::uint8_t> m_view_life;
// ---- Effects
AnimationLibrary m_animations;
std::vector<Flash> m_flashes;
```

### 5.5. Brush — события клавиш и опрос мыши

```cpp
void tick_brush(Sandbox::App& app) {
    for (const Sandbox::KeyEvent& key : m_brush_keys.events()) {            // дискретное — событием
        if (key.action != GLFW_PRESS) continue;
        if (key.key >= GLFW_KEY_1 && key.key <= GLFW_KEY_5) m_brush = static_cast<Material>(key.key - GLFW_KEY_1 + 1);
        if (key.key == GLFW_KEY_LEFT_BRACKET) m_radius = std::max(m_radius - 1, 0);
        if (key.key == GLFW_KEY_RIGHT_BRACKET) m_radius = std::min(m_radius + 1, 12);
    }
    const Sandbox::FrameInput& in = app.input();                            // непрерывное — опросом
    if (in.down[0] || in.down[1]) {
        m_brush_out.emit(PaintEvent{
            .x = static_cast<std::int32_t>(in.mouse_world.x / cell_size),
            .y = static_cast<std::int32_t>(in.mouse_world.y / cell_size),
            .material = static_cast<std::uint32_t>(in.down[0] ? m_brush : Material::Empty),
            .radius = static_cast<std::uint32_t>(m_radius)});
    }
}
```

Кисть **не меняет сетку сама** — она только отправляет намерение. Сетку меняет Simulation.
Поэтому любой другой источник (погода, скрипт заклинания, сетевой игрок) может рисовать тем же событием.

### 5.6. Weather — второй производитель и ручное расписание

```cpp
void tick_weather(Sandbox::App& app) {
    const es::Tick tick = app.tick();
    m_weather_out.emit(PaintEvent{.x = 45, .y = 2, .material = /*Sand*/ 1, .radius = 1});
    if (tick % 2 == 0) {
        m_weather_out.emit(PaintEvent{.x = 170, .y = 2, .material = /*Water*/ 2, .radius = 1});
    }
    // «Молния» каждые 8 секунд. Отложенных событий в шине нет — расписание вручную по номеру тика.
    if (tick % 240 == 120) {
        m_weather_out.emit(PaintEvent{.x = 108, .y = 84, .material = /*Fire*/ 5, .radius = 1});
    }
}
```

В графе видно, что у `sand.paint` два производителя: `consumes sand.paint <- Brush, Weather`.
Simulation не различает их — ей неважно, кто рисует.

### 5.7. Simulation — физика и слияние изменений

**Шаг 1. Применить команды рисования** из прошлого тика (кругом радиуса `r`).

**Шаг 2. Физика.** Проход снизу вверх, направление строки чередуется:

```cpp
++m_stamp;
for (int y = grid_h - 1; y >= 0; --y) {
    const bool left_to_right = (y + static_cast<int>(m_stamp)) % 2 == 0;
    for (int step = 0; step < grid_w; ++step) {
        const int x = left_to_right ? step : grid_w - 1 - step;
        const std::size_t i = index(x, y);
        if (m_moved[i] == m_stamp) continue;    // частица уже сдвинулась в этом тике
        switch (m_cells[i]) {
            case Material::Sand:
            case Material::Water: /* вниз → по диагонали → (вода) вбок до 8 клеток */ break;
            case Material::Fire:  /* поджечь соседнее дерево, погаснуть от воды или по таймеру */ break;
            default: break;
        }
    }
}
```

| Приём | Зачем |
|---|---|
| снизу вверх | упавшая частица не обрабатывается повторно в той же строке |
| чередование направления | иначе песок и вода «текут» в одну сторону |
| `m_moved[i] == m_stamp` | частицу, сдвинутую вбок по ходу прохода, не двигаем второй раз. Штамп — номер тика, очищать массив не нужно |
| вода растекается до 8 клеток | при одной клетке за тик вода стоит горкой, как песок |
| песок тонет в воде (`can_enter`) | `swap` песка и воды — вода всплывает сама |

Растекание воды (найдено на скриншоте — вода стояла горкой):

```cpp
} else if (m == Material::Water) {
    constexpr int dispersion = 8;
    for (const int dir : {side, -side}) {
        int reach = 0;
        while (reach < dispersion && can_enter(x + dir * (reach + 1), y, m)) ++reach;
        if (reach > 0) {
            swap_cells(i, index(x + dir * reach, y));
            break;
        }
    }
}
```

**Шаг 3. Изменения — пачкой событий, каждая клетка не больше раза.**

Частица может за тик сдвинуться, а на её место упасть другая: клетка меняется дважды.
Отправлять оба изменения бессмысленно, важно конечное состояние. Поэтому — dirty-флаги:

```cpp
void mark_dirty(std::size_t i) {
    if (m_dirty[i] == 0) {                  // первый раз в этом тике
        m_dirty[i] = 1;
        m_dirty_list.push_back(static_cast<std::uint32_t>(i));
    }
}
...
for (const std::uint32_t i : m_dirty_list) {
    m_sim_changes.emit(CellChangedEvent{.x = static_cast<std::int32_t>(i % grid_w),
                                        .y = static_cast<std::int32_t>(i / grid_w),
                                        .material = static_cast<std::uint16_t>(m_cells[i]),
                                        .life = m_life[i]});
    m_dirty[i] = 0;
}
m_dirty_list.clear();
```

> Это ручная реализация политики **Coalesced** (слияние по ключу), которая была в плане
> EventSystem. В движке это должно делать сама шина — см. раздел 9.

**Начальный мир** строится через тот же `set_cell()` и тоже помечается dirty. Поэтому первый тик
отправляет весь начальный мир одной пачкой событий, и View получает его без отдельного кода.

### 5.8. View — зеркало мира только из событий

```cpp
void tick_view() {
    // Читаем только колонки — так и задумывалась SoA-раскладка.
    const auto xs = m_view_changes.column<&CellChangedEvent::x>();
    const auto ys = m_view_changes.column<&CellChangedEvent::y>();
    const auto materials = m_view_changes.column<&CellChangedEvent::material>();
    const auto lives = m_view_changes.column<&CellChangedEvent::life>();
    for (std::size_t k = 0; k < xs.size(); ++k) {
        const std::size_t i = index(xs[k], ys[k]);
        m_view[i] = static_cast<Material>(materials[k]);
        m_view_life[i] = static_cast<std::uint8_t>(lives[k]);
    }
}
```

```
Simulation.m_cells ──(dirty)──▶ sand.cell_changed [x][y][material][life] ──▶ View.m_view ──▶ render()
   «истина»                          4 плотные колонки                      «зеркало»
```

Зачем зеркало, если сетка лежит рядом? Это модель будущего движка:
- симуляция может жить в другом потоке или процессе (сервер), а рендер получает только изменения;
- по этим же событиям можно обновлять текстуру мира, сохранять реплей, слать по сети;
- рендер физически не может сломать состояние симуляции.

### 5.9. Effects — анимация вспышек

```cpp
// setup(): процедурный спрайт-лист 4×1 и незацикленный клип
Image sheet(64, 16, Colors::transparent);
for (int f = 0; f < 4; ++f) {
    const int inset = f * 2;                          // каждый кадр меньше и прозрачнее
    sheet.fill_rect(f * 16 + inset, inset, 16 - 2 * inset, 16 - 2 * inset,
                    Color::from_rgba(0xFFF3B0FF).with_alpha(static_cast<std::uint8_t>(230 - f * 50)));
}
m_spark_texture = app.renderer().create_texture(sheet);
m_spark_clip = m_animations.add(AnimationClip{
    .name = "fx.spark", .frames = make_grid_frames({.columns = 4, .rows = 1}, 0, 4, 0.06f), .looping = false});
```

```cpp
void tick_effects(Sandbox::App& app) {
    for (const IgnitedEvent& ignition : m_effects_ignited.events()) {
        if (m_flashes.size() >= 96) break;                        // свой лимит на число вспышек
        m_flashes.push_back(Flash{.position = {...}, .anim = AnimationState::start(m_spark_clip)});
    }
    for (Flash& flash : m_flashes) {                              // AoS → по одному (см. раздел 9)
        advance_animations(std::span(&flash.anim, 1), m_animations, app.tick_seconds());
    }
    std::erase_if(m_flashes, [&](const Flash& f) { return is_finished(f.anim, m_animations); });
}
```

- Кадр вспышки выбирается при отрисовке: `.uv = current_uv(flash.anim, m_animations)`.
- `is_finished()` работает только для незацикленных клипов: вспышка доиграла — удаляем.
- **Нюанс.** `advance_animations` принимает плотный `span<AnimationState>`. Здесь состояние лежит
  внутри `Flash` (AoS), поэтому приходится вызывать по одному элементу. Colony хранит состояния
  отдельным массивом и обновляет их одним вызовом — так и задумано.

### 5.10. Отрисовка FallingSand

```cpp
void render(Sandbox::App& app, Renderer2D& r) override {
    r.fill_rect({{0.0f, 0.0f}, world_size()}, Color::from_rgba(0x0E1016FF), -10);   // фон, слой -10
    for (int y = 0; y < grid_h; ++y)
        for (int x = 0; x < grid_w; ++x)
            if (m_view[index(x, y)] != Material::Empty)
                r.fill_rect({{x * cell_size, y * cell_size}, {cell_size, cell_size}},
                            material_color(m_view[index(x, y)], x, y, m_view_life[index(x, y)]), 0);
    for (const Flash& flash : m_flashes)                                               // слой 5
        r.draw(SpriteInstance{.position = flash.position, .size = {cell_size * 6, cell_size * 6},
                              .uv = current_uv(flash.anim, m_animations), .texture = m_spark_texture, .layer = 5});
    r.draw_rect({m - half, glm::vec2(half * 2.0f)}, 1.5f, ..., 10);                    // контур кисти
}
```

- Каждая клетка — отдельный квад через белую текстуру. Все квады одного слоя и одной
  текстуры, поэтому весь мир — **один draw call**, сколько бы клеток ни было.
- Цвет клетки «шумит» по хешу координат, чтобы песок не был плоским:

```cpp
const auto h = static_cast<std::uint32_t>(x * 73856093) ^ static_cast<std::uint32_t>(y * 19349663);
const float shade = 0.92f + 0.16f * static_cast<float>(h % 97) / 96.0f;   // ±8% яркости
```

- Огонь мерцает: цвет зависит от `life`, поэтому клетка огня помечается dirty каждый тик.

---

## 6. Colony — упрощённый RimWorld

### 6.1. Что это

Карта 48×30 тайлов по 16 пикселей. Десять колонистов сами выбирают работу: рубят деревья и
добывают камень, несут добычу на склад. Срубленные деревья отрастают. Игрок сажает деревья (ЛКМ)
и кладёт камни (ПКМ). Модуль «Хроника» пишет историю колонии в консоль.

Что демонстрирует:
- **цепочки событий через 6 модулей** и законный цикл;
- **общие данные + события**: как модули делят состояние, пока нет ECS;
- **SoA-данные сущностей** и обновление всех анимаций одним вызовом;
- **атлас текстур**: ~1500 спрайтов кадра — около 4 draw call'ов (атлас и белая текстура чередуются по слоям);
- **хронику** — зачаток генерируемой истории в духе Dwarf Fortress.

### 6.2. Граф модулей

```
                 platform.key, platform.mouse_button
  ┌──────────┐ ───────────────────────────────────▶ ┌────────┐
  │ Platform │                                      │ Orders │
  └──────────┘                                      └───┬────┘
                                                        │ colony.order_place
                                                        ▼
                        colony.resource_harvested  ┌─────────┐  colony.resource_spawned
            ┌─────────────────────────────────────▶│  World  │──────────────────────────┐
            │                                      └────┬────┘                          │
            │                    colony.resource_depleted│                              │
            │                 ┌────────────────────────┬─┴──────────────────────┐       │
            │                 ▼                        ▼                        ▼       ▼
            │            ┌─────────┐  job_assigned ┌───────────┐          ┌───────────┐
            │            │  Jobs   │──────────────▶│ Colonists │          │ Chronicle │
            │            └─────────┘               └─────┬─────┘          └───────────┘
            │                 ▲   resource_harvested     │                      ▲
            └─────────────────┴──────────────────────────┤                      │
                                                         │ item_delivered       │ milestone
                                                         ▼                      │
                                                    ┌─────────┐                 │
                                                    │ Economy │─────────────────┘
                                                    └─────────┘

 Цикл: World ─depleted─▶ Colonists ─harvested─▶ World   (и через Jobs)
 module order: Platform Orders [cycle: World] [cycle: Jobs] [cycle: Colonists] [cycle: Economy] [cycle: Chronicle]
```

Цикл законный: ответ всегда приходит на следующем тике. Но `module_order()` помечает как
«циклические» и Economy с Chronicle, которые просто стоят ниже цикла. Это недостаток отчёта
(раздел 9): цикл нужно показывать одним узлом и упорядочивать остальное.

Контракты в коде — это буквально граф:

```cpp
const es::ModuleId orders = bus.declare_module("Orders")
    .consumes<Sandbox::KeyEvent>().consumes<Sandbox::MouseButtonEvent>()
    .produces<PlaceOrderEvent>();
const es::ModuleId world = bus.declare_module("World")
    .consumes<PlaceOrderEvent>().consumes<ResourceHarvestedEvent>()
    .produces<ResourceSpawnedEvent>().produces<ResourceDepletedEvent>();
const es::ModuleId jobs = bus.declare_module("Jobs")
    .consumes<ResourceHarvestedEvent>().consumes<ResourceDepletedEvent>()
    .produces<JobAssignedEvent>();
const es::ModuleId colonists = bus.declare_module("Colonists")
    .consumes<JobAssignedEvent>().consumes<ResourceDepletedEvent>()
    .produces<ResourceHarvestedEvent>().produces<ItemDeliveredEvent>();
const es::ModuleId economy = bus.declare_module("Economy")
    .consumes<ItemDeliveredEvent>().produces<MilestoneEvent>();
const es::ModuleId chronicle = bus.declare_module("Chronicle")
    .consumes<MilestoneEvent>().consumes<ResourceDepletedEvent>().consumes<ResourceSpawnedEvent>();
```

### 6.3. События

| Событие | Поля | Кто пишет → кто читает | Смысл |
|---|---|---|---|
| `colony.order_place` | tile_x, tile_y, kind | Orders → World | игрок хочет дерево/камень |
| `colony.resource_spawned` | resource, tile_x, tile_y, kind, regrown | World → Chronicle | ресурс появился |
| `colony.job_assigned` | colonist, resource | Jobs → Colonists | «иди работай с ресурсом» |
| `colony.resource_harvested` | colonist, resource, kind, amount | Colonists → World, Jobs | добыта единица |
| `colony.resource_depleted` | resource, tile_x, tile_y, kind | World → Jobs, Colonists, Chronicle | ресурс кончился |
| `colony.item_delivered` | colonist, kind, amount | Colonists → Economy | предмет на складе |
| `colony.milestone` | kind, total | Economy → Chronicle | на складе ещё +10 |

Все события — AoS: их единицы за тик. Пример:

```cpp
struct ResourceHarvestedEvent {
    std::uint32_t colonist = 0;
    std::uint32_t resource = 0;
    std::uint32_t kind = 0;          // ResourceKind (enum class : uint32_t)
    std::uint32_t amount = 0;

    static constexpr std::string_view event_name = "colony.resource_harvested";
    using fields = es::Fields<es::Field<"colonist", &ResourceHarvestedEvent::colonist>,
                              es::Field<"resource", &ResourceHarvestedEvent::resource>,
                              es::Field<"kind", &ResourceHarvestedEvent::kind>,
                              es::Field<"amount", &ResourceHarvestedEvent::amount>>;
};
```

> **Нюанс.** `kind` передаётся как `uint32_t`, а не как `enum class`: `field_kind_of<>()` в схеме
> распознаёт только скаляры, перечисление было бы «opaque». Для скриптов и инспектора
> лучше явный числовой тип.

### 6.4. Хронология одной работы по тикам

Как событие путешествует между модулями. Каждая стрелка — «отправлено в тике N, прочитано в N+1».

```
тик  модуль      что происходит                                         событие
───  ──────────  ─────────────────────────────────────────────────────  ─────────────────────────
 0   Jobs        колонист 3 свободен, ближайшее дерево #12 → резерв      ──▶ job_assigned(3, 12)
 1   Jobs        колонист 3 ещё Idle, но m_awaiting_job[3] → пропуск
 1   Colonists   читает job_assigned → state = ToResource, цель = #12
 2   Jobs        видит state != Idle → m_awaiting_job[3] = false
2…k  Colonists   идёт к дереву (48 px в игровую секунду)
 k   Colonists   дошёл → state = Working, таймер 45 тиков
k+45 Colonists   таймер вышел → несёт бревно, state = ToStockpile        ──▶ resource_harvested(3, 12)
k+46 World       дерево #12: amount 3 → 2
k+46 Jobs        снимает резерв с #12 — дерево снова можно назначить
 …   Colonists   дошёл до склада                                        ──▶ item_delivered(3, Tree)
 …   Economy     logs 9 → 10 — порог десятка                            ──▶ milestone(Tree, 10)
 …   Chronicle   «[day 1, 06:56] the stockpile now holds 10 logs»
```

Реальный вывод хроники:

```
[day 1, 00:00] Chronicle: 10 settlers founded a colony among 27 trees and 11 rocks
[day 1, 06:19] Chronicle: a tree at (11, 5) is gone
[day 1, 06:21] Chronicle: a tree at (10, 1) is gone
[day 1, 06:56] Chronicle: the stockpile now holds 10 logs
[day 1, 10:07] Chronicle: a rock at (9, 2) is gone
[day 1, 13:30] Chronicle: the stockpile now holds 10 stones
```

Одна игровая минута = один тик: `day = tick / 1440 + 1`, `hh = (tick / 60) % 24`, `mm = tick % 60`.

### 6.5. Общие данные и события — кто что делает

Пока нет ECS, модули **читают** общие данные напрямую (позиции колонистов, список ресурсов),
а **сообщают об изменениях** событиями. Это граница, которую потом займёт ECS:

```
      события — «что случилось»                 общие данные — «как есть сейчас»
   ┌─────────────────────────────┐          ┌─────────────────────────────────────┐
   │ job_assigned, harvested,    │          │ m_resources[]  (владелец: World)     │
   │ depleted, delivered, ...    │          │ m_pos[], m_state[] (владелец: Colon.)│
   └─────────────────────────────┘          │ m_stock[]      (владелец: Economy)   │
                                            └─────────────────────────────────────┘
   Jobs читает m_resources, m_pos, m_state, m_stock — но не пишет в них.
   Пишет в чужие данные только тот модуль, который ими владеет.
```

### 6.6. World — ресурсы, индексы и отрастание

```cpp
/// Создаёт ресурс; возвращает индекс или -1, если тайл занят.
/// Индексы не переиспользуются: без ID с поколением старые события указали бы на новый ресурс.
std::int64_t spawn(glm::ivec2 t, ResourceKind kind, bool announce, bool regrown = false) {
    if (t.x < 0 || t.y < 0 || t.x >= map_w || t.y >= map_h || in_stockpile(t)) return -1;
    for (const Resource& r : m_resources) if (r.alive && r.tile == t) return -1;
    const int amount = kind == ResourceKind::Tree ? 3 : 5;
    m_resources.push_back(Resource{.tile = t, .kind = kind, .amount = amount, .max_amount = amount, .alive = true});
    m_reserved_by.push_back(-1);
    ...
}
```

**Важный нюанс — почему индексы не переиспользуются.** Представь: дерево #12 срублено, его слот
отдан новому камню, а в шине ещё летит `job_assigned(3, 12)`, отправленное до рубки. Колонист
пойдёт к камню, думая, что это дерево. Это та самая проблема «устаревшей ссылки», из-за которой
нужны **ID с поколением** (индекс + счётчик). Пока ECS их не даёт, Colony только добавляет ресурсы.

Обработка добычи и отложенное отрастание:

```cpp
for (const ResourceHarvestedEvent& h : m_world_harvested.events()) {
    Resource& res = m_resources[h.resource];
    if (!res.alive) continue;                        // двое добыли последнюю единицу в один тик
    res.amount -= static_cast<int>(h.amount);
    if (res.amount <= 0) {
        res.alive = false;
        m_world_depleted.emit(ResourceDepletedEvent{...});
        if (res.kind == ResourceKind::Tree) {
            // Отложенных событий в шине нет — очередь отрастания ведётся вручную.
            m_regrowth.push_back(Regrowth{.due = now + 900, .near = res.tile});   // 900 тиков = 30 с
        }
    }
}
std::erase_if(m_regrowth, [&](const Regrowth& g) {
    if (g.due > now) return false;
    spawn(g.near + glm::ivec2{jitter(m_rng), jitter(m_rng)}, ResourceKind::Tree, true, true);
    return true;                                     // занятый тайл — саженец просто не вырос
});
```

`m_regrowth` — это ручная политика **Scheduled** («событие на тик N»).

### 6.7. Jobs — распределение работы

```cpp
void tick_jobs() {
    for (const auto& h : m_jobs_harvested.events()) m_reserved_by[h.resource] = -1;   // освободить
    for (const auto& d : m_jobs_depleted.events())  m_reserved_by[d.resource] = -1;

    const ResourceKind wanted = m_stock[0] <= m_stock[1] ? ResourceKind::Tree : ResourceKind::Rock;
    for (std::size_t c = 0; c < m_pos.size(); ++c) {
        if (m_state[c] != State::Idle) { m_awaiting_job[c] = false; continue; }
        if (m_awaiting_job[c]) continue;    // задание уже отправлено, Colonists получит его в следующем тике

        // ближайший свободный ресурс; «ненужный» вид считается в 2.5 раза дальше
        ... score = distance * (kind == wanted ? 1.0f : 2.5f) ...

        if (best >= 0) {
            m_reserved_by[best] = static_cast<std::int32_t>(c);
            m_jobs_out.emit(JobAssignedEvent{.colonist = c, .resource = best});
            m_awaiting_job[c] = true;
        }
    }
}
```

**Нюанс `m_awaiting_job`.** Это прямое следствие семантики N+1. Jobs отправил задание в тике 0,
но Colonists увидит его только в тике 1, а в самом тике 1 Jobs выполняется **раньше** Colonists и
всё ещё видит колониста свободным. Без флага он назначил бы ему второе задание.

```
тик 0:  Jobs: Idle → emit job, awaiting=1     Colonists: (ничего)
тик 1:  Jobs: Idle, awaiting=1 → пропуск       Colonists: читает job → ToResource
тик 2:  Jobs: state≠Idle → awaiting=0          Colonists: идёт
```

**Резерв** (`m_reserved_by`) не даёт двум колонистам идти к одному дереву. Он снимается после
каждой добытой единицы, поэтому дерево с 3 единицами может обслужить несколько колонистов по очереди.

### 6.8. Colonists — конечный автомат на SoA-массивах

Данные колонистов — **структура массивов**, как будущие компоненты ECS:

```cpp
std::vector<glm::vec2> m_pos;
std::vector<glm::vec2> m_goal;
std::vector<State> m_state;              // Idle, ToResource, Working, ToStockpile
std::vector<std::uint32_t> m_target;
std::vector<int> m_work_timer;
std::vector<ResourceKind> m_carrying;
std::vector<bool> m_facing_left;
std::vector<AnimationState> m_anim;      // плотный массив — важно для анимаций
```

Автомат состояний:

```
                job_assigned (цель жива)
        ┌──────┐ ────────────────────▶ ┌────────────┐  дошёл   ┌─────────┐
        │ Idle │                        │ ToResource │ ───────▶ │ Working │
        └──────┘ ◀──────────────────── └────────────┘          └────┬────┘
           ▲       resource_depleted (цель исчезла)  ◀───────────────┤
           │                                                        │ таймер 45/70 тиков
           │  дошёл до склада                                       │ emit resource_harvested
           │  emit item_delivered      ┌─────────────┐              │
           └───────────────────────────│ ToStockpile │ ◀────────────┘
                                       └─────────────┘
```

Выбор клипа и **одно** обновление всех анимаций:

```cpp
// в цикле по колонистам:
const ClipId clip = m_state[c] == State::Working ? m_work : (m_state[c] == State::Idle ? m_idle : m_walk);
if (m_anim[c].clip != clip) m_anim[c] = AnimationState::start(clip);   // смена клипа — с первого кадра

// после цикла:
advance_animations(m_anim, m_anims, dt);   // все колонисты — один проход по плотному массиву
```

Клипы берутся из одного атласа (сетка 8×2):

```cpp
m_walk = m_anims.add({.name = "colonist.walk", .frames = make_grid_frames({.columns = 8, .rows = 2}, WalkFirst, 4, 0.12f)});
m_work = m_anims.add({.name = "colonist.work", .frames = make_grid_frames({.columns = 8, .rows = 2}, WorkFirst, 2, 0.2f)});
m_idle = m_anims.add({.name = "colonist.idle", .frames = make_grid_frames({.columns = 8, .rows = 2}, Idle, 1, 1.0f)});
```

Реакция на исчезновение цели — через событие, а не проверку чужих данных:

```cpp
for (const ResourceDepletedEvent& d : m_colonists_depleted.events()) {
    for (std::size_t c = 0; c < m_pos.size(); ++c) {
        if (m_target[c] == d.resource && (m_state[c] == State::ToResource || m_state[c] == State::Working)) {
            m_state[c] = State::Idle;   // кто-то успел раньше
        }
    }
}
```

### 6.9. Economy и Chronicle

```cpp
void tick_economy() {
    for (const ItemDeliveredEvent& item : m_economy_delivered.events()) {
        std::uint32_t& stock = m_stock[item.kind];
        const std::uint32_t before = stock;
        stock += item.amount;
        if (stock / 10 != before / 10) {                     // перешли через десяток
            m_economy_milestones.emit(MilestoneEvent{.kind = item.kind, .total = stock});
        }
    }
}
```

Хроника **видит только события**, не данные модулей. Это важно для будущей генерации сюжета:
история строится из того, что произошло, а не из снимков состояния.

```cpp
void tick_chronicle(es::Tick now) {
    for (const MilestoneEvent& m : m_chronicle_milestones.events())
        std::println("[{}] Chronicle: the stockpile now holds {} {}", game_time(now), m.total,
                     m.kind == 0 ? "logs" : "stones");
    for (const ResourceDepletedEvent& d : m_chronicle_depleted.events()) ...
    for (const ResourceSpawnedEvent& s : m_chronicle_spawned.events()) ...
}
```

> Сейчас хроника печатает в консоль. Правильное место для этого — политика доставки **Log**
> (журнал, который сохраняется в сейв), запланированная для EventSystem.

### 6.10. Отрисовка Colony

**Процедурный атлас 128×32** (8×2 клеток по 16 px) — вся графика игры в одной текстуре:

```
 x:  0      16     32     48     64     80     96     112
   ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
 0 │трава │земля │склад │дерево│камень│бревно│камень│      │   строка 0: мир и предметы
   ├──────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┤
16 │ходьба│ходьба│ходьба│ходьба│работа│работа│покой │      │   строка 1: колонист
   └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
```

```cpp
Image make_atlas() {
    Image atlas(128, 32, Colors::transparent);
    auto cell = [&](int index, int x, int y, int w, int h, std::uint32_t rgba) {
        atlas.fill_rect((index % 8) * 16 + x, (index / 8) * 16 + y, w, h, Color::from_rgba(rgba));
    };
    cell(TreeSprite, 7, 9, 2, 6, 0x6B4423FF);   // ствол
    cell(TreeSprite, 3, 1, 10, 9, 0x2E6B2EFF);  // крона
    ...
}
```

UV клеток считаются **один раз**:

```cpp
UvRect atlas_uv(int index) {
    // make_grid_frames возвращает vector — в цикле отрисовки это аллокация на каждый спрайт,
    // поэтому UV клеток считаются один раз (в RendererSystem не хватает SpriteSheetGrid::cell_uv()).
    static const std::vector<AnimationFrame> cells = make_grid_frames({.columns = 8, .rows = 2}, 0, 16, 1.0f);
    return cells[static_cast<std::size_t>(index)].uv;
}
```

> **Нюанс, найденный по ходу.** Первая версия вызывала `make_grid_frames` на каждый спрайт каждого
> кадра: около 1500 аллокаций `vector` за кадр. Работало, но впустую. Отсюда пожелание к API —
> получать UV клетки без аллокации.

**Слои** задают порядок наложения:

| Слой | Что | Текстура |
|---|---|---|
| -10 | земля, трава, пол склада | атлас |
| -8 | предметы на складе | атлас |
| 0 | деревья и камни | атлас |
| 1 | полоски «осталось добыть» | белая |
| 2 | колонисты (анимация, отражение по направлению) | атлас |
| 3 | предмет над головой у несущего | атлас |
| 4 | линии заданий (J) | белая |

```cpp
for (std::size_t i = 0; i < m_pos.size(); ++i) {
    SpriteInstance s = sprite(m_pos[i], Idle, 2);
    s.uv = current_uv(m_anim[i], m_anims);                                   // кадр анимации
    s.flip = m_facing_left[i] ? SpriteFlip::X : SpriteFlip::None;            // смотрит влево
    r.draw(s);
    if (m_state[i] == State::ToStockpile)
        r.draw(sprite(m_pos[i] + glm::vec2{0.0f, -11.0f},
                      m_carrying[i] == ResourceKind::Tree ? LogItem : StoneItem, 3));
    if (m_show_jobs && m_state[i] != State::Idle)
        r.draw_line(m_pos[i], m_goal[i], 1.0f, Color{255, 255, 255, 90}, 4);
}
```

Благодаря атласу и сортировке `LayerThenTexture` кадр из ~1500 спрайтов рисуется **несколькими
draw call'ами**: по одному на каждый непрерывный участок «слой + текстура» (атлас или белая).

---

## 7. RendererSystem на практике

Сводка того, как рендер используется в Sandbox:

| Возможность | Где | Как |
|---|---|---|
| `Renderer2D::create()` → `std::expected` | App | ошибка шейдера → исключение с текстом |
| `create_texture(Image)` | вспышки, атлас Colony | изображения генерируются кодом, файлов нет |
| `Camera2D` + `screen_to_world` | App | камера мира, зум к курсору, мышь в мир |
| второй `begin/end` с экранной камерой | App | оверлей в пикселях экрана |
| `fill_rect`, `draw_rect`, `draw_line` | все | клетки, панели, контур кисти, линии заданий |
| `SpriteInstance` + атлас + `uv` | Colony | тайлы, ресурсы, колонисты |
| `SpriteFlip::X` | Colony | колонист смотрит в сторону движения |
| `layer` | обе игры | порядок наложения без ручной сортировки |
| `AnimationLibrary` / `advance_animations` / `current_uv` | обе игры | колонисты (SoA), вспышки (AoS) |
| `is_finished` | FallingSand | удаление доигравших вспышек |
| `last_stats().draw_calls` | App | в заголовок окна |
| `Image` + `flip_vertically` | App | скриншот |

**Как кадр превращается в draw call'ы:**

```
render():  r.draw(...) × N   ──▶  SpriteBatch.submit()        (просто копия в массив)
r.end():   build()               ──▶ ключ = (слой, текстура)
                                  ──▶ LSD radix sort (1–2 прохода в типичном кадре)
                                  ──▶ 4 вершины на спрайт + DrawCommand на участок
           GPU                    ──▶ glBufferData (orphaning) + glDrawElements × участков
```

---

## 8. Замеры производительности

### 8.1. Sandbox (RelWithDebInfo, AMD + Mesa, монитор 2560×1440 @ 240 Гц)

| Игра | Кадров | Время | Тиков | FPS | CPU процесса | CPU на кадр |
|---|---|---|---|---|---|---|
| FallingSand | 3000 | 12.6 с | 375 | 240 (vsync) | 0.71 с user + 0.16 с sys | ≈ 0.24 мс |
| Colony | 3000 | 12.6 с | 375 | 240 (vsync) | 0.45 с user + 0.14 с sys | ≈ 0.15 мс |

- Обе игры **упираются в vsync**: 240 fps — это частота монитора, а не предел движка.
  Кадр 4.17 мс, из них процессор занят примерно 0.15–0.25 мс, остальное ожидание.
- CPU на кадр включает всё: тики симуляции (в среднем 1 тик на 8 кадров), сборку батча,
  драйвер, запуск и завершение процесса. Это верхняя оценка.
- 375 тиков за 12.6 с при 30 тиках/с — фиксированный тик работает точно.

Статистика каналов FallingSand за те же 375 тиков:

| Канал | Событий всего | Пик за тик | Отброшено | Память |
|---|---|---|---|---|
| `sand.paint` | 563 | 3 | 0 | 8 KB |
| `sand.cell_changed` | 294 661 | 3 240 | 0 | 639 KB |
| `sand.ignited` | 796 | 13 | 0 | 1 KB |

- `cell_changed` — в среднем ~790 событий за тик, в пике 3240. 639 KB — это два буфера
  (pending + ready) с резервом на всю сетку (27 280 клеток × 12 байт × 2).
- Отброшенных событий нет — бюджеты подобраны с запасом.

Статистика Colony за 375 тиков (12.5 игровых секунд):

| Канал | Событий | Пик за тик |
|---|---|---|
| `colony.job_assigned` | 21 | 10 (первый тик: все 10 колонистов) |
| `colony.resource_harvested` | 13 | 1 |
| `colony.item_delivered` | 11 | 1 |

Colony нагружает шину слабо — это нормально для логики уровня RimWorld. Её ценность в сложности
связей, а не в объёме.

### 8.2. Микробенчмарки модулей (Release, для сравнения)

**EventSystem:**

| Замер | Результат |
|---|---|
| `emit` AoS | ~300–600 млн событий/с |
| `emit` SoA | ~200–290 млн событий/с |
| `vector::push_back` (базовая линия) | ~1.0–1.35 млрд/с |
| чтение всех полей AoS / SoA | 1.34 / 1.27 млрд событий/с |
| получение писателя (`bus.writer<E>()`) | ~27 нс — поэтому один раз в `setup()` |

**RendererSystem:**

| Замер | Результат |
|---|---|
| `advance_animations` | ~500 млн состояний/с |
| сборка батча, 1 текстура | 40–85 млн спрайтов/с |
| сборка батча, 16 текстур, 131 тыс. спрайтов | 5.3 мс (было 12.1 мс до radix sort) |
| полный кадр на GPU, 131 тыс. спрайтов, 16 текстур | 7.1 мс (было 13.8 мс) |

**Что это значит для Sandbox.** FallingSand отправляет в пике 3240 событий за тик: для шины это
~10 мкс. Сетка — до ~27 тыс. квадов за кадр: для батча ~0.5 мс в худшем случае. Запас огромный:
узкое место сейчас — vsync, а не движок.

---

## 9. Что нужно дорабатывать и почему

Каждый пункт найден при написании игр, а не придуман заранее.

### EventSystem

| # | Проблема | Где проявилась | Что сделать |
|---|---|---|---|
| 1 | ввод доставляется только в тиках: на паузе клики копятся | App, 3.5 | второй домен времени: шина кадра или `Delivery::Frame` |
| 2 | нет отложенных событий | Weather, World (6.6) | политика `Scheduled`: `emit_at(tick, event)` |
| 3 | нет слияния по ключу | Simulation dirty-флаги (5.7) | политика `Coalesced` |
| 4 | `module_order()` метит «циклическими» всех ниже цикла | Colony (6.2) | конденсация сильно связных компонент (Tarjan) |
| 5 | много шаблонного кода: 19 полей reader/writer | Colony setup | `ModuleContext`: модуль сам получает всё по контракту |
| 6 | ссылки на сущности — голые индексы | World (6.6) | ID с поколением (ECS) |
| 7 | история пишется в консоль | Chronicle (6.9) | политика `Log` с сохранением |

### RendererSystem

| # | Проблема | Где проявилась | Что сделать |
|---|---|---|---|
| 1 | нет текста | статус в заголовке, полоски вместо чисел | атлас глифов на `stb_truetype` |
| 2 | нельзя обновить текстуру через `Renderer2D` | FallingSand рисует ~27 тыс. квадов | `update_texture(handle, image / область)` |
| 3 | статичная геометрия пересобирается каждый кадр | тайлы Colony, клетки FallingSand | кэшированные буферы чанков (важно для вокселей) |
| 4 | `advance_animations` только для плотного `span` | вспышки FallingSand (5.9) | перегрузка с проекцией/шагом |
| 5 | UV клетки сетки только через `vector` | `atlas_uv` (6.10) | `SpriteSheetGrid::cell_uv(index)` |
| 6 | нет чтения окна в `Image` | скриншот (3.9) | `Renderer2D::read_default_framebuffer()` |

### WindowSystem

| # | Проблема | Обход в App |
|---|---|---|
| 1 | нет мыши и колеса | `glfwGetCursorPos`, `glfwSetScrollCallback` + статический указатель |
| 2 | нет `set_title`, размера framebuffer'а, vsync, времени | `glfwSetWindowTitle`, `glfwGetFramebufferSize`, `glfwGetTime` |
| 3 | один слушатель клавиш, Esc жёстко закрывает окно | единственный `onKeyPress` занят App |
| 4 | `glfwTerminate` в деструкторе | ложные утечки под LeakSanitizer |

### Главный архитектурный вывод

```
            сейчас (Sandbox)                          дальше (движок)
  ┌────────────────────────────────┐        ┌──────────────────────────────────┐
  │ события   — что случилось   ✔  │        │ EventSystem (+Scheduled,         │
  │ общие     — vector<> в классе ✘│  ───▶  │   Coalesced, Log, Frame)         │
  │ данные      игры, голые индексы│        │ ECS: компоненты + ID с поколением│
  │ модули    — методы одного класса│        │ модули — отдельные системы с     │
  │                                │        │   ModuleContext                  │
  └────────────────────────────────┘        └──────────────────────────────────┘
```

Шина событий уже справляется со своей ролью. Следующее недостающее звено — **ECS**: без него
модули делят данные через поля одного класса, а ссылки на сущности ненадёжны.

---

## 10. Шпаргалка по API

### EventSystem

```cpp
// событие
struct E { int32_t a; float b;
    static constexpr std::string_view event_name = "module.event";
    static constexpr es::Layout layout = es::Layout::SoA;              // необязательно, по умолчанию AoS
    using fields = es::Fields<es::Field<"a", &E::a>, es::Field<"b", &E::b>>; };

// контракт модуля
es::ModuleId m = bus.declare_module("Name")
    .produces<E>(es::ChannelConfig{.reserve = 256, .max_events_per_tick = 4096})
    .consumes<F>();

// доступ (один раз в setup)
es::EventWriter<E> out = bus.writer<E>(m);
es::EventReader<F> in  = bus.reader<F>(m);

// тик
out.emit(E{...});                                   // → false, если отброшено бюджетом
for (const F& f : in.events()) { ... }             // AoS
auto col = in.column<&F::field>();                  // SoA: span<const T>
bus.advance_tick();                                  // делает App

// инструменты
bus.build_graph().to_text() / .to_dot() / .module_order() / .unconsumed_events()
bus.channel_at(i).stats()                            // readable, peak_per_tick, total_dropped, ...
```

### RendererSystem

```cpp
auto renderer = Renderer2D::create().value();
TextureHandle tex = renderer.create_texture(image);                  // или load_texture(path)

renderer.begin(camera);                                              // Camera2D
renderer.draw(SpriteInstance{.position, .size, .pivot, .rotation, .uv, .color, .texture, .layer, .flip});
renderer.fill_rect(rect, color, layer);
renderer.draw_rect(rect, thickness, color, layer);
renderer.draw_line(a, b, thickness, color, layer);
RenderStats s = renderer.end();                                      // quads, draw_calls, texture_binds

// анимации
ClipId clip = library.add({.name = "x", .frames = make_grid_frames({.columns, .rows}, first, count, dur)});
std::vector<AnimationState> states(n, AnimationState::start(clip));
advance_animations(states, library, dt);
sprite.uv = current_uv(states[i], library);
is_finished(states[i], library);
```

### Sandbox

```cpp
class MyGame final : public Sandbox::Game {
    glm::vec2 world_size() const override;
    void setup(Sandbox::App& app) override;       // модули, writer/reader, текстуры
    void tick(Sandbox::App& app) override;        // app.tick(), app.tick_seconds(), app.input()
    void render(Sandbox::App& app, Renderer2D& r) override;
};

// ввод: события Sandbox::KeyEvent / Sandbox::MouseButtonEvent (модуль "Platform")
//       или app.input().mouse_world / .down[0..2] / .pressed[0..2]
// клавиши каркаса: Space, = / -, WASD, колесо, F1, Esc
// запуск: --frames N  --screenshot file.png
```
