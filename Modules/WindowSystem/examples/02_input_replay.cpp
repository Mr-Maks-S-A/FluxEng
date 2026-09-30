/**
 * @example 02_input_replay.cpp
 * InputState без окна: запись ввода и его воспроизведение по кадрам.
 * Так тестируется игровая логика, зависящая от ввода, — без дисплея и без GLFW.
 */

#include <WindowSystem/Input.hpp>
#include <WindowSystem/Listeners.hpp>

#include <print>
#include <vector>

namespace {

constexpr int key_space = 32; // GLFW_KEY_SPACE
constexpr int key_d = 68;     // GLFW_KEY_D

struct RecordedEvent {
    int frame;
    int key;
    int action;
};

} // namespace

int main() {
    // Запись: пробел нажат и отпущен в кадре 1, D зажата с кадра 2 по 4.
    const std::vector<RecordedEvent> recording = {
        {1, key_space, WindowSystem::action_press}, {1, key_space, WindowSystem::action_release},
        {2, key_d, WindowSystem::action_press},     {4, key_d, WindowSystem::action_release},
    };

    WindowSystem::InputState input;
    WindowSystem::Listeners<int, int> key_events;
    key_events.subscribe([](int key, int action) { std::println("  event: key {} action {}", key, action); });

    float player_x = 0.0f;
    int jumps = 0;
    std::size_t cursor = 0;
    for (int frame = 0; frame < 6; ++frame) {
        input.begin_frame();
        while (cursor < recording.size() && recording[cursor].frame == frame) {
            input.on_key(recording[cursor].key, recording[cursor].action);
            key_events.emit(recording[cursor].key, recording[cursor].action);
            ++cursor;
        }
        // Игровая логика читает только InputState — ей всё равно, откуда события.
        if (input.pressed(key_space)) ++jumps;
        if (input.down(key_d)) player_x += 1.0f;
        std::println("frame {}: x = {}, jumps = {}", frame, player_x, jumps);
    }
}
