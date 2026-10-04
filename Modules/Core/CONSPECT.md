# Core — конспект для изучения и переписывания

`src/include/Core/{Core,App,Actions,PlatformEvents,FixedStep}.hpp`, `src/code/{App,Actions}.cpp` (~1100 строк). Зависит от `RuntimeSystem`, `EventSystem`, `JobSystem`, `MemorySystem`, `WindowSystem`, `RendererSystem`.

## 1. Зачем и главная идея

`Core` — **каркас клиентского приложения**: окно + рендер + ввод + цикл кадров и тиков. Игра — наследник `Core::Game`, у которого есть хуки жизненного цикла; всё остальное делает `Core::App`. Вся «тяжёлая» часть без графики вынесена в `RuntimeSystem`: `App` **владеет `Runtime`** и добавляет к нему окно, GPU-устройство, рендеры, камеру, ввод.

Разделение ответственности:
- `RuntimeSystem` — что нужно и серверу (шина, задачи, память, тик, модули);
- `Core` — только то, что нужно клиенту (окно, рендер, ввод, камера, пауза/скорость клавишами, оверлей шины).

## 2. Карта файлов

| Файл | Что |
|---|---|
| `Core.hpp` | `Core::run<G>(config, argc, argv)` — точка входа `main` |
| `App.hpp/.cpp` | `AppConfig`, `parse_args`, `FrameInput`, `Game`, `App` |
| `Actions.hpp/.cpp` | `Binding`, `ActionId`, `ActionMap` — ввод через действия, перепривязка из файла |
| `PlatformEvents.hpp` | `KeyEvent`, `MouseButtonEvent` — события шины от модуля «Platform» |
| `FixedStep.hpp` | алиас на `RuntimeSystem::FixedStep` (совместимость) |

## 3. Как устроено

### 3.1 `Game` — контракт игры
Хуки (в порядке вызова за жизнь программы):

| Хук | Когда |
|---|---|
| `configure(app)` | до `initialize()`: здесь добавляются модули (`app.add_module<M>()`) |
| `setup(app)` | после `initialize()`: создание ресурсов, GPU-объектов, мира |
| `frame(app, seconds)` | **каждый кадр** до тиков (идёт и на паузе): ввод, интерполяция, рисование в текстуры |
| `tick(app)` | каждый тик симуляции — вызывается как `tick_callback` Runtime **после** `Module::tick` всех модулей |
| `render_3d` / `render` / `render_overlay` | отрисовка: 3D; 2D в камере мира; 2D в экранных координатах (HUD) |
| `status()` | строка для заголовка окна |
| `shutdown(app)` | при выходе, после `wait_idle`, до модулей |
`world_size()` задаёт мир для начальной камеры.

### 3.2 Кадр `App::run` (последовательность — главное, что нужно понимать)
```
RunScope · game.configure · runtime.initialize · game.setup · set_tick_callback(game.tick)
печать графа событий и порядка модулей
цикл:
  window.poll_events()          ← ввод кадра; клавиши/мышь → подписчики → шина (platform.key / platform.mouse_button)
  frame_dt = min(now − last, 0.25)
  poll_input(dt)                ← камера (WASD/колесо), курсор в мире
  device.begin_frame            ← до Game::frame: игра может рисовать в текстуры
  runtime.begin_frame(dt)       ← сброс арены кадра, Module::frame
  game.frame(dt)
  runtime.update(dt)            ← bus.advance_frame; тики: модули → game.tick → advance_tick → смена арен
  отрисовка: render_3d → render(2D, камера мира) → render_overlay + оверлей шины (экранные координаты)
  device.end_frame · swap_buffers
  выход по max_frames / max_ticks (--frames, --ticks) или закрытию окна
scope.finish → wait_idle → game.shutdown → модули в обратном порядке
```
`RunScope` — RAII: при исключении в любой точке гарантирует корректное завершение (ожидание GPU, `shutdown` игры и модулей).

### 3.3 `AppConfig` и `parse_args`
Заголовок, размер окна, `visible` (false — скрытое окно для тестов), `ticks_per_second`, лимиты `max_frames`/`max_ticks` (включают lockstep: **один тик на кадр** — детерминированные прогоны), `screenshot`, `pause_key` (по умолчанию пробел; 3D-игре нужен для прыжка, `0` — без паузы), `threads`, `camera_controls`, `clear_rgba`, `ui_font_size`, `backend` (`--backend gl|vulkan`), `validation`, размеры арен, `profile_modules`, `extra_args` — **что Core не разобрал, отдаётся игре**.
`parse_args` дополняет конфиг из командной строки (`--frames`, `--ticks`, `--screenshot`, `--threads`, `--backend`, `--validation`).
**Ловушка:** пробел ставит паузу по умолчанию — в примерах и тестах с программным нажатием пробела выставляй `pause_key = 0`.

### 3.4 Модуль «Platform»
В конструкторе `App` объявляется модуль `Platform` (`produces<KeyEvent>`, `produces<MouseButtonEvent>`, бюджет 1024/тик), берутся писатели. Колбэки окна → события шины **по порядку**, включая внедрённые `Window::inject_*` между кадрами (флаги кадра `InputState` их бы потеряли). Координаты мыши переводятся в мир камерой. `on_key` заодно обрабатывает паузу, `=`/`-` (скорость ×1…×8), F1 (отчёт по шине), Esc.

### 3.5 Порядок полей `App` (важно для деструкторов)
`m_window` → `m_device` → рендеры → шрифты → **`m_runtime`** (после устройства и рендеров: при уничтожении модули, а с ними их GPU-ресурсы, уходят **раньше устройства**). Если переставишь, упадёшь при выходе.

### 3.6 `ActionMap` — ввод через действия
- `declare("jump", "Прыжок")` → `ActionId`; повтор имени возвращает прежний id.
- `bind(id, Binding::key(GLFW_KEY_SPACE))` (несколько привязок на действие), `unbind`, `unbind_all`, `conflicts(id)` (кто ещё использует ту же клавишу).
- Запросы: `down/pressed/released(input, id)` — достаточно любой привязки; `axis(input, positive, negative)` → −1/0/+1.
- `apply(text)` — перепривязка из текста (`имя = КЛАВИША, КЛАВИША`, `#` комментарии): **атомарно** (ошибка со строкой → ничего не применено), заменяет привязки только упомянутых действий. `serialize()` — готовый файл настроек. `binding_name` / `parse_binding` — имена клавиш («W», «SPACE», «MOUSE_LEFT»).
- Работает поверх `WindowSystem::InputState` (в тестах заполняется вручную).

## 4. Как использовать

```cpp
struct MyGame final : Core::Game {
    void configure(Core::App& app) override { app.add_module<PhysicsModule>(); }
    void setup(Core::App& app) override { /* создать ресурсы */ }
    void tick(Core::App& app) override { /* шаг симуляции */ }
    void render(Core::App& app, RendererSystem::Renderer2D& r) override { /* 2D в мире */ }
};
int main(int argc, char** argv) {
    return Core::run<MyGame>({.title = "MyGame", .ticks_per_second = 30.0}, argc, argv);
}
// ./MyGame --ticks 120 --screenshot out.png --backend vulkan --threads 4
```
Где это в проекте: **все игры** в `Sandbox/` (кроме консольных `CoopScribe` и `WorldJournal`) наследуют `Core::Game`; `FirstSpell`, `RuneCell2`, `ModuleProof` используют `ActionMap`.

## 5. Что менять осторожно

- **Порядок кадра** (§3.2) и порядок полей `App` (§3.5). Особенно: `device.begin_frame` до `Game::frame`, `runtime.begin_frame` до `game.frame`, `update` после.
- `Game::tick` вызывается из `Runtime::tick` через `set_tick_callback`, а не как модуль — значит, событие, отправленное в нём, видно в следующем тике, как и у модулей.
- Модули добавляются **только в `configure`** (до `initialize`); после — `RuntimeError`.
- `frame_dt` обрезан до 0.25 с: при долгой паузе (отладчик) симуляция не «догоняет».
- Оверлей шины рисуется столбиками по `ChannelStats::readable` (лог-шкала) — это отладка, не часть игры; он рисуется в `render_overlay`-камере, поверх HUD.
- `Core::run` ловит `std::exception` и возвращает 1: ошибки создания окна/шейдера видны в stderr, не падение.

## 6. Упражнения для переписывания

1. Напиши консольный `Core::HeadlessApp` (без окна) на одном `Runtime` — что из `App` придётся убрать, что останется?
2. Вынеси обработку клавиш паузы/скорости в отдельный модуль `DebugControls`, чтобы `App::on_key` остался чистым.
3. Добавь геймпад в `Binding::Device` и ось `axis` для стика.
4. Сделай хранение настроек: сохранить `actions.serialize()` в файл при выходе и `apply` при старте, обработав ошибку со строкой.
5. Перенеси интерполяцию (`tick_alpha`) в готовый хелпер `Interpolated<T>` для рендера.

## 7. Тесты и примеры

`Modules/Core/tests` (4 файла: `test_fixed_step`, `test_actions`, …), 2 примера; интеграционные тесты `Integration.cpu/gpu: test_core_app`; GPU-тесты идут под xvfb.
