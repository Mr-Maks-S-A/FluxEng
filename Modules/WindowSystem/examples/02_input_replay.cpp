/**
 * @example 02_input_replay.cpp
 * Окно без экрана (Headless) и запись ввода: внедрённые события идут тем же путём, что и события ОС,
 * поэтому игровая логика, зависящая от ввода, тестируется без дисплея, без GPU и без GLFW.
 */

#include <InputSystem/InputLog.hpp>
#include <WindowSystem/Window.hpp>

#include <print>

using namespace InputSystem;

int main() {
    auto created = WindowSystem::Window::create({.backend = WindowSystem::WindowBackend::Headless});
    if (!created) {
        std::println(stderr, "{}", created.error());
        return 1;
    }
    WindowSystem::Window& window = *created;

    // Запись: подписка на все события ввода окна. Номер кадра ведёт игра.
    InputLog log;
    std::uint32_t frame = 0;
    window.events().input.subscribe([&](const InputEvent& event) { log.record(frame, event); });

    // «Игрок»: пробел нажат и отпущен в кадре 1, D зажата с кадра 2 по 4.
    const auto play = [&](std::uint32_t f) {
        if (f == 1) {
            window.inject_key(Key::Space, Transition::Press);
            window.inject_key(Key::Space, Transition::Release);
        }
        if (f == 2) window.inject_key(Key::D, Transition::Press);
        if (f == 4) window.inject_key(Key::D, Transition::Release);
    };

    float player_x = 0.0f;
    int jumps = 0;
    for (frame = 0; frame < 6; ++frame) {
        window.poll_events();
        play(frame);
        // Игровая логика читает только InputState — ей всё равно, откуда события.
        if (window.input().pressed(Key::Space)) ++jumps;
        if (window.input().down(Key::D)) player_x += 1.0f;
        std::println("frame {}: x = {}, jumps = {}", frame, player_x, jumps);
    }

    // Воспроизведение той же записи в чистое состояние даёт тот же результат.
    InputState replayed;
    float replay_x = 0.0f;
    int replay_jumps = 0;
    for (std::uint32_t f = 0; f < 6; ++f) {
        replayed.begin_frame();
        for (const LoggedEvent& e : log.events_of_frame(f)) replayed.apply(e.event);
        if (replayed.pressed(Key::Space)) ++replay_jumps;
        if (replayed.down(Key::D)) replay_x += 1.0f;
    }
    std::println("replay: x = {}, jumps = {} ({} events recorded)", replay_x, replay_jumps, log.size());
    return replay_x == player_x && replay_jumps == jumps && jumps == 1 && player_x == 2.0f ? 0 : 1;
}
