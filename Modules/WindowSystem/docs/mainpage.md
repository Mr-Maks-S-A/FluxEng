# WindowSystem — окно и ввод FluxEng {#mainpage}

WindowSystem открывает окно с OpenGL-контекстом и отдаёт движку ввод: опросом за кадр
(InputState) и подписками на события (Listeners). Всё остальное в движке, включая Core,
больше не вызывает GLFW напрямую для ввода, заголовка, времени и размеров.
Подключение: `#include <WindowSystem/Window.hpp>`, CMake-цель `engine::WindowSystem`.

## Создание

```cpp
auto created = WindowSystem::Window::create({.title = "Game", .width = 1280, .height = 720});
if (!created) {
    std::println(stderr, "{}", created.error());   // текст причины: нет дисплея, нет OpenGL 3.3…
    return 1;
}
WindowSystem::Window& window = *created;
```

Раньше конструктор мог вернуть «наполовину созданное» окно, а счётчик окон расходился,
если glad не загрузился. Теперь create() либо возвращает готовое окно, либо ошибку,
и при любой ошибке всё уже освобождено.

## Кадр

```cpp
while (!window.should_close()) {
    window.poll_events();                               // 1. новый кадр ввода + события ОС
    const WindowSystem::InputState& in = window.input();
    if (in.pressed(GLFW_KEY_SPACE)) jump();             // 2. опрос
    if (in.down(GLFW_KEY_D)) move_right();
    const WindowSystem::Size fb = window.framebuffer_size();
    render(fb.width, fb.height);                        // 3. отрисовка
    window.swap_buffers();                              // 4. показ
}
```

## Ввод опросом: InputState

| Вопрос | Метод | Когда `true` |
|---|---|---|
| зажата? | `down(key)` | с нажатия до отпускания |
| нажали в этом кадре? | `pressed(key)` | один кадр; нажатие и отпускание внутри кадра тоже ловится |
| отпустили в этом кадре? | `released(key)` | один кадр |
| мышь | `mouse_down / mouse_pressed / mouse_released` | так же |
| курсор | `cursor()`, `cursor_delta()`, Window::cursor_in_framebuffer() | пиксели окна / framebuffer'а (HiDPI) |
| колесо | `scroll()` | сумма за кадр |
| текст | `text()` | Unicode-символы за кадр |

InputState не зависит от GLFW: его можно заполнять из записи и тестировать без окна
(@ref 02_input_replay.cpp). При потере фокуса всё «отпускается», поэтому клавиши не залипают.

**ZII.** `InputState{}` — ничего не нажато; пустое `Window{}` — все запросы безопасны.

## Ввод подписками: WindowEvents

```cpp
auto id = window.events().key.subscribe([&](int key, int action) { ... });
window.events().scroll.subscribe([](double dx, double dy) { ... });
window.events().key.unsubscribe(id);
```

На каждое событие — сколько угодно подписчиков (раньше был один `std::function onKeyPress`,
и его занимал Core). Отписаться можно изнутри обработчика.

События: `key`, `mouse_button`, `cursor`, `scroll`, `character`, `framebuffer_resized`, `focus`.

## Что ещё изменилось

| Было | Стало |
|---|---|
| Esc всегда закрывал окно | решает игра; для примеров есть `WindowConfig::close_on_escape` |
| `update()` = swap + poll в конце кадра | `poll_events()` в начале и `swap_buffers()` в конце |
| окно само вызывало `glViewport` при resize | окно не рисует; подпишитесь на `framebuffer_resized` |
| нет мыши, колеса, заголовка, времени, vsync | `input()`, `set_title()`, `time()`, `set_vsync()`, `content_scale()` |
| глобальный класс `Window` | `WindowSystem::Window` |
| перемещённое окно ломало указатель GLFW | состояние в куче, адрес стабилен |

## Тесты без дисплея

Тесты InputState и Listeners не требуют окна. Тесты Window создают скрытое окно,
а если дисплея нет — пропускаются с сообщением. Для CI на Linux: `xvfb-run ctest`.
Методы `inject_*` отправляют событие тем же путём, что и ОС, — ими же можно
воспроизводить записанный ввод.

## Замеры

| Замер | Время |
|---|---|
| begin_frame + 8 событий клавиш | ~20 нс |
| `down(key)` | ~1 нс |
| рассылка события 8 подписчикам | ~20 нс |

## Примеры

- @ref 01_window_input.cpp — окно, опрос и подписки
- @ref 02_input_replay.cpp — InputState без окна, воспроизведение записи
