# WindowSystem — окно и платформа FluxEng {#mainpage}

WindowSystem открывает окно, ведёт кадр и превращает события платформы в **собственные события ввода движка**
(InputSystem). В заголовках модуля нет ни GLFW, ни glad: платформа — деталь бэкенда за интерфейсом `IWindowBackend`.
Подключение: `#include <WindowSystem/Window.hpp>`, CMake-цель `engine::WindowSystem` (тянет `engine::InputSystem`).

```
 ОС ─► IWindowBackend (GLFW | Headless | ваш) ─► InputEvent ─► Window ─► InputState (опрос за кадр)
                                                                      └► WindowEvents (подписки)
```

## Создание

```cpp
auto created = WindowSystem::Window::create({.title = "Game", .width = 1280, .height = 720});
if (!created) {
    std::println(stderr, "{}", created.error());   // текст причины: нет дисплея, нет OpenGL 3.3…
    return 1;
}
WindowSystem::Window& window = *created;
```

`create()` либо возвращает готовое окно, либо ошибку; при любой ошибке всё уже освобождено.

## Платформы

| `WindowConfig::backend` | Что это | Когда нужно |
|---|---|---|
| `WindowBackend::Glfw` (по умолчанию) | окно ОС: Windows, Linux (X11, Wayland), macOS; контекст OpenGL или окно для Vulkan; геймпады | игра |
| `WindowBackend::Headless` | окно без экрана: размеры из конфигурации, события только внедрённые, графики нет | CI без дисплея, выделенный сервер, автотесты ввода |

Своя платформа (SDL, Android, консоль, веб) — ещё одна реализация `IWindowBackend` (`WindowSystem/Backend.hpp`): она переводит
события ОС в `InputSystem::InputEvent`, остальной код не меняется. Коды клавиш GLFW переводятся **в одном месте**
(`GlfwKeyMap.hpp`), и тест проверяет перевод для всех кодов GLFW (отображение полное и взаимно однозначное).

## Кадр

```cpp
using InputSystem::Key;
while (!window.should_close()) {
    window.poll_events();                               // 1. новый кадр ввода + события платформы
    const InputSystem::InputState& in = window.input();
    if (in.pressed(Key::Space)) jump();                 // 2. опрос
    if (in.down(Key::D)) move_right();
    const WindowSystem::Size fb = window.framebuffer_size();
    render(fb.width, fb.height);                        // 3. отрисовка
    window.swap_buffers();                              // 4. показ
}
```

Опрос, действия (`ActionMap`), команда ввода за тик и запись ввода описаны в документации **InputSystem**: окно лишь доставляет события.

## События подписками: WindowEvents

```cpp
auto id = window.events().key.subscribe([&](InputSystem::Key key, InputSystem::Transition t) { ... });
window.events().input.subscribe([&](const InputSystem::InputEvent& e) { log.record(frame, e); });   // любое событие, в том числе геймпад
window.events().key.unsubscribe(id);
```

Порядок: сначала обновляется `input()`, затем typed-подписчики, затем общая подписка `input`. Подписчиков — сколько угодно;
отписаться можно изнутри обработчика. События: `key`, `mouse_button`, `cursor`, `scroll`, `character`, `framebuffer_resized`, `focus`, `input`.

## Внедрение ввода

`inject(event)` и `inject_key / inject_mouse_button / inject_cursor / inject_scroll / inject_char` посылают событие **тем же путём**,
что и ОС (состояние, затем подписчики). Так работают боты (Sandbox/CardDuel кликает мышью), автотесты и воспроизведение записи.
Вместе с Headless-окном это даёт тесты игровой логики без дисплея, без GPU и без GLFW.

## Геймпады

Бэкенд GLFW опрашивает геймпады после `glfwPollEvents` и отдаёт только изменения: подключение, кнопки (раскладка «как у Xbox»),
оси (стики −1…+1, курки 0…1). Игрок — номер 0…3. Без железа ту же цепочку проверяют внедрённые `GamepadInput`.

## Графический API

`ClientApi::OpenGL` — контекст OpenGL, функции загружены внутри окна; **заголовок glad подключайте сами** там, где вызываете GL.
`ClientApi::None` — окно без контекста для Vulkan: `create_vulkan_surface(instance)` и `Window::vulkan_instance_extensions()`.
`native_handle()` возвращает `void*` (у GLFW — `GLFWwindow*`); он нужен только коду, который сам говорит с платформой.

## ZII и перемещение

Пустое `Window{}` — все запросы безопасны (размеры 0, `should_close() == true`). Перемещённое окно продолжает работать:
состояние лежит в куче, обработчики платформы видят актуальный объект.

## Тесты без дисплея

Headless-окно, Listeners и перевод кодов GLFW не требуют дисплея. Тесты окна GLFW создают скрытое окно, а без дисплея
пропускаются с сообщением (для CI на Linux: `xvfb-run ctest`).

## Что изменилось по сравнению с прежней версией

| Было | Стало |
|---|---|
| `Window.hpp` включал glad и GLFW; игры писали `GLFW_KEY_W` | заголовки без платформы; `InputSystem::Key::W` |
| события `(int key, int action)` с кодами GLFW | `(Key, Transition)`: собственные стабильные коды |
| `WindowSystem::InputState`, `action_press` | `InputSystem::InputState`, `Transition::Press` |
| `native_handle()` возвращал `GLFWwindow*` | `void*` |
| платформа одна (GLFW), дисплей обязателен | бэкенды GLFW и Headless, шов для новых |
| геймпадов нет | подключение, кнопки, оси |
| `Window::time()` через `glfwGetTime` | монотонные часы, не зависят от платформы |

## Примеры

- @ref 01_window_input.cpp — окно GLFW, опрос и подписки
- @ref 02_input_replay.cpp — Headless-окно, запись и воспроизведение ввода
