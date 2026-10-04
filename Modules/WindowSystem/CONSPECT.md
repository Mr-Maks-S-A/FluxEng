# WindowSystem — конспект для изучения и переписывания

`src/include/WindowSystem/{Window,Input,Listeners}.hpp` и `src/code/Window.cpp` (~810 строк). Зависимостей от модулей нет; снаружи — GLFW, glad (OpenGL 4.5 в `ExternalLibrary`), опционально Vulkan-заголовки (`FLUX_WINDOW_VULKAN`).

## 1. Зачем и главная идея

Единственное место, которое знает про GLFW. Всё остальное в движке получает от окна:
- **ввод опросом за кадр** (`InputState`): «зажата ли клавиша», «нажали в этом кадре»;
- **ввод подписками** (`WindowEvents` + `Listeners`): каждое событие по порядку;
- размеры, заголовок, время, режим курсора, поверхность Vulkan.

`InputState` **не зависит от GLFW и окна** — его можно заполнять из записи или вручную в тесте. Окно только пересылает в него события.

## 2. Карта файлов

| Файл | Что |
|---|---|
| `Input.hpp` | `InputState` (биты клавиш/кнопок, курсор, колесо, текст), коды действий `action_press/release/repeat` |
| `Listeners.hpp` | `Listeners<Args...>` (подписка/отписка/рассылка), `ListenerId` |
| `Window.hpp` | `Window`, `WindowConfig`, `WindowEvents`, `Size`, `ClientApi`, `CursorMode` |
| `Window.cpp` | `Window::Impl`, счётчик окон/инициализация GLFW, колбэки, инъекция событий, Vulkan-поверхность |

## 3. Как устроено

### 3.1 `InputState`
- Три `bitset<512>` на клавиши (`down`, `pressed`, `released`) и три `bitset<8>` на кнопки мыши. Коды — как у GLFW.
- `apply(...)`: `press` → `down` + `pressed`; `release` → сброс `down`, `released`; `repeat` — состояние не меняется. Поэтому `pressed` ловит **и нажатие+отпускание внутри одного кадра** (бит `pressed` остаётся до `begin_frame`).
- `begin_frame()` сбрасывает «в этом кадре»-биты, колесо, текст, и запоминает курсор как предыдущий (для `cursor_delta`). Состояние `down` сохраняется между кадрами.
- Текст — до 32 символов за кадр, лишние отбрасываются. `on_focus_lost()` отпускает всё (`released |= down`, `down.reset()`), чтобы клавиши не залипали.
- ZII: `InputState{}` — ничего не нажато.

### 3.2 `Listeners<Args...>`
Вектор `{ListenerId, std::function}` + счётчик. Тонкости:
- `emit` идёт **по индексу до размера на начало рассылки**: подписчик, добавленный изнутри обработчика, получит **следующее** событие.
- `unsubscribe` изнутри обработчика не стирает запись, а обнуляет `handler` и ставит `m_dirty`; удаление — после рассылки (`--m_emitting == 0`). Так итерация не ломается и порядок подписки сохраняется.
- `ListenerId{0}` — «нет подписки» (ZII).

### 3.3 `Window` и `Impl`
- Состояние окна лежит в `unique_ptr<Impl>` (куча), а в GLFW передаётся `glfwSetWindowUserPointer(&impl)` — поэтому `Window` можно **перемещать**, а колбэки продолжают видеть актуальный объект.
- Общая точка входа событий: `Impl::key/mouse_button/cursor/scroll/character` — **сначала `input`, потом подписчики**. И настоящие события ОС (через колбэки GLFW), и внедрённые (`inject_*`) идут одним путём.
- Счётчик окон `g_open_windows`: `glfwInit` — с первым окном, `glfwTerminate` — с последним. `create()` либо возвращает готовое окно, либо ошибку (`std::expected<Window, std::string>`); при любой ошибке всё освобождено и счётчик возвращён.
- `ClientApi::OpenGL`: контекст core нужной версии (по умолчанию 3.3), `glad` грузит функции; `ClientApi::None`: `GLFW_NO_API` для Vulkan, `swap_buffers()` ничего не делает, vsync — свойство swapchain'а. `create_vulkan_surface(instance)` возвращает `VkSurfaceKHR` как целое.
- `poll_events()`: `input.begin_frame()`, затем `glfwPollEvents()` — обработчики вызываются внутри, после возврата `input()` описывает этот кадр.
- `cursor_in_framebuffer()` пересчитывает курсор из координат окна в пиксели framebuffer'а (HiDPI).
- `CursorMode::Captured` → `GLFW_CURSOR_DISABLED` + «сырое» движение мыши, если поддерживается (обзор в 3D).
- Пустое `Window{}` безопасно: размеры 0, `should_close() == true`, `input()` возвращает статический пустой `InputState`.

### 3.4 Инъекция событий
`inject_key/mouse_button/cursor/scroll/char` — события «как от ОС»: нужны тестам и автоигре (`--autoplay`). **Ловушка:** внедрённое событие между кадрами обновляет `InputState`, но `poll_events()` вызовет `begin_frame()` и сбросит `pressed/released`. Поэтому `Core::App` берёт клавиши и кнопки мыши **подпиской** (она получает каждое событие по порядку), а `InputState` использует для удержания и курсора.

## 4. Как использовать

```cpp
auto created = WindowSystem::Window::create({.title = "Game", .width = 1280, .height = 720});
if (!created) { std::println(stderr, "{}", created.error()); return 1; }
WindowSystem::Window& window = *created;

auto id = window.events().key.subscribe([&](int key, int action) { /* каждое событие по порядку */ });

while (!window.should_close()) {
    window.poll_events();
    const auto& in = window.input();
    if (in.pressed(GLFW_KEY_SPACE)) jump();
    if (in.down(GLFW_KEY_D))        move_right();
    render(window.framebuffer_size().width, window.framebuffer_size().height);
    window.swap_buffers();
}
window.events().key.unsubscribe(id);
```
В тестах без окна: `InputState s; s.on_key(GLFW_KEY_W, action_press); s.pressed(GLFW_KEY_W)`.

Где это в проекте: `Core::App` (окно, ввод, подписки на `key`/`mouse_button`); `Core::ActionMap` читает `InputState`; `RendererSystem` через `native_handle()` и `create_vulkan_surface` подключается к окну.

## 5. Что менять осторожно

- **Порядок «сначала `input`, потом подписчики»** — подписчик может спросить `input()` и увидеть уже обновлённое состояние.
- **Счётчик окон:** новое место создания/уничтожения окна обязано вызывать `acquire/release_platform`, иначе утечка или преждевременный `glfwTerminate`.
- `glad` должен подключаться **строго до GLFW** (`Window.hpp`, комментарий clang-format off).
- Не храни ссылку на `Impl` или `GLFWwindow*` дольше `Window` — окно можно переместить (Impl остаётся, но `handle` обнуляется в деструкторе).
- `InputState::key_count = 512` при `GLFW_KEY_LAST = 348`: коды выше игнорируются (`test` возвращает `false`).
- `close_on_escape` — для примеров; в игре решает сама игра.

## 6. Упражнения для переписывания

1. Замени GLFW на SDL3, оставив публичный API: что придётся заменить в `Impl` и колбэках, а что останется нетронутым (`InputState`, `Listeners`)?
2. Добавь геймпад в `InputState` (кнопки, оси с мёртвой зоной) и привязку в `Core::ActionMap`.
3. Реализуй запись ввода в файл и воспроизведение: `InputState` не зависит от окна — какие события записывать, чтобы воспроизведение было побитово точным?
4. Добавь `Listeners::subscribe_once` и тест на подписку/отписку изнутри обработчика.
5. Сделай `inject_*` честными: пусть они добавляются в очередь и применяются на `poll_events()` (после `begin_frame`) — что сломается в `Core::App`?

## 7. Тесты и примеры

`Modules/WindowSystem/tests` (4 файла: ввод, подписки, окно), примеры `01_*` и `02_input_replay.cpp` (ввод без окна). Тесты окна запускаются под xvfb (метка GPU).
