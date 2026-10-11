/**
 * @file 02_record_and_replay.cpp
 * @brief Запись ввода и воспроизведение: реплей, автотест игровой логики без дисплея, воспроизведение бага.
 *
 * Игровая логика читает только InputState — ей всё равно, откуда события: окно, запись или бот.
 */

#include <InputSystem/InputSystem.hpp>

#include <cstdio>
#include <vector>

using namespace InputSystem;

namespace {

/// «Игра»: бежит вправо, пока зажата D, прыгает по пробелу.
struct Game {
    int x = 0;
    int jumps = 0;
    void update(const InputState& input) {
        if (input.down(Key::D)) ++x;
        if (input.pressed(Key::Space)) ++jumps;
    }
};

} // namespace

int main() {
    // 1. «Живая» игра: события приходят от платформы и пишутся в журнал.
    InputLog log;
    Game live;
    InputState state;
    for (std::uint32_t frame = 0; frame < 8; ++frame) {
        state.begin_frame();
        std::vector<InputEvent> from_platform;
        if (frame == 1) from_platform.push_back(KeyInput{Key::D, Transition::Press, Modifiers::None});
        if (frame == 3) from_platform.push_back(KeyInput{Key::Space, Transition::Press, Modifiers::None});
        if (frame == 4) from_platform.push_back(KeyInput{Key::Space, Transition::Release, Modifiers::None});
        if (frame == 6) from_platform.push_back(KeyInput{Key::D, Transition::Release, Modifiers::None});
        for (const InputEvent& e : from_platform) {
            state.apply(e);
            log.record(frame, e);
        }
        live.update(state);
    }
    const std::vector<std::byte> file = log.to_bytes(); // это можно сохранить на диск
    std::printf("записано %zu событий, %zu байт; живая игра: x = %d, прыжков %d\n", log.size(), file.size(), live.x, live.jumps);

    // 2. Воспроизведение: та же логика, ввод из файла.
    const auto replay = InputLog::from_bytes(file);
    if (!replay) {
        std::printf("запись повреждена: %s\n", replay.error().c_str());
        return 1;
    }
    Game replayed;
    InputState replay_state;
    for (std::uint32_t frame = 0; frame < 8; ++frame) {
        replay_state.begin_frame();
        for (const LoggedEvent& e : replay->events_of_frame(frame)) replay_state.apply(e.event);
        replayed.update(replay_state);
    }
    std::printf("воспроизведение:      x = %d, прыжков %d\n", replayed.x, replayed.jumps);

    // 3. Повреждённая запись отвергается, а не разбирается наугад.
    auto broken = file;
    broken[20] ^= std::byte{0xFF};
    std::printf("повреждённая запись: %s\n", InputLog::from_bytes(broken) ? "принята (плохо!)" : "отвергнута");

    return replayed.x == live.x && replayed.jumps == live.jumps && !InputLog::from_bytes(broken) ? 0 : 1;
}
