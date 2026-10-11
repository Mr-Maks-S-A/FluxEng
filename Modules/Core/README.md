# Core — слой приложения

То, что стоит между `main()` и игровой логикой: окно, рендер и шина событий в одном `App`, цикл «кадры + фиксированный тик»,
ввод как события и как **действия**, отладка (граф событий, F1). Игра — наследник `Core::Game` и одна строка в `main()`.

Подключение: `#include <Core/Core.hpp>`, цель `engine::Core`. Зависит от всех «системных» модулей: MemorySystem, JobSystem,
EventSystem, RendererSystem, WindowSystem (и GLFW для кодов клавиш).

```cpp
class MyGame final : public Core::Game {
    void setup(Core::App& app) override { /* объявить модули и события, привязать действия */ }
    void frame(Core::App& app, float seconds) override { /* ввод кадра, камера, данные на GPU */ }
    void tick(Core::App& app) override { /* шаг симуляции, фиксированная частота */ }
    void render_3d(Core::App&, RendererSystem::Renderer3D&) override {}
    void render_overlay(Core::App&, RendererSystem::Renderer2D&) override {}
};
int main(int argc, char** argv) { return Core::run<MyGame>({.title = "MyGame", .ticks_per_second = 60.0}, argc, argv); }
```

Кадр: `poll_events → frame → тики (tick + advance_tick) → clear → render_3d → render → render_overlay → показ`.
Аргументы командной строки (`--frames`, `--ticks`, `--threads`, `--screenshot`, `--backend gl|vulkan`) разбирает `parse_args`;
`--ticks N` включает lockstep — ровно один тик на кадр, прогон детерминирован.

## Ввод через действия (`InputSystem/ActionMap.hpp`)

Действия живут в модуле `InputSystem` (раньше был `Core::ActionMap`): игра привязывает клавиши и кнопки к именам действий и
спрашивает про действия; перепривязка файлом игрока — `to_text()` / `from_text()`.

```cpp
InputSystem::ActionMap actions;
actions.bind("jump", InputSystem::Key::Space);
if (actions.pressed("jump", app.window().input())) start_jump();
```

Пример: `02_minimal_game.cpp` (скрытое окно, 120 тиков, события, программное нажатие клавиши).
Тесты: `CoreTests` (FixedStep, аргументы); интеграционные — в `Tests/integration`.
