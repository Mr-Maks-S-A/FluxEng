/**
 * @file 03_commands.cpp
 * @brief Команда ввода за тик: клавиатура и геймпад дают одни и те же 16 байт — симуляции всё равно, откуда ввод.
 *
 * InputCommand можно отправить по сети (NetSystem::Lockstep), записать и воспроизвести. Симуляция читает только команду.
 */

#include <InputSystem/InputSystem.hpp>

#include <cstdio>

using namespace InputSystem;

namespace {

/// Симуляция: целочисленная, читает только команду.
struct Sim {
    int x = 0, y = 0, casts = 0;
    void step(const InputCommand& previous, const InputCommand& current) {
        x += current.axes[0] * 10 / InputCommand::kAxisMax; // ±10 за тик на полном отклонении (только целые числа)
        y += current.axes[1] * 10 / InputCommand::kAxisMax;
        if (button_pressed(previous, current, 0)) ++casts; // заклинание — по фронту, а не пока зажато
    }
};

} // namespace

int main() {
    ActionMap actions;
    actions.bind("cast", Key::Space).bind("cast", GamepadButton::X);
    actions.bind_keys("move_x", Key::A, Key::D).bind_axis("move_x", GamepadAxis::LeftX, 1.0f, 0.0f);
    actions.bind_keys("move_y", Key::W, Key::S).bind_axis("move_y", GamepadAxis::LeftY, 1.0f, 0.0f);

    CommandLayout layout; // одинаковая у всех узлов сети и зашита в игру
    layout.button("cast").axis("move_x").axis("move_y");

    // Два игрока делают одно и то же: один на клавиатуре, другой на геймпаде.
    InputState keyboard, pad;
    keyboard.begin_frame();
    keyboard.apply(KeyInput{Key::D, Transition::Press, Modifiers::None});
    keyboard.apply(KeyInput{Key::Space, Transition::Press, Modifiers::None});
    pad.begin_frame();
    pad.apply(GamepadConnectionInput{0, true});
    pad.apply(GamepadAxisInput{0, GamepadAxis::LeftX, 1.0f});
    pad.apply(GamepadButtonInput{0, GamepadButton::X, Transition::Press});

    const InputCommand from_keyboard = sample_command(actions, keyboard, layout);
    const InputCommand from_pad = sample_command(actions, pad, layout);
    std::printf("клавиатура: кнопки %#x, ось X %d; геймпад: кнопки %#x, ось X %d; команды %s\n", from_keyboard.buttons, from_keyboard.axes[0],
                from_pad.buttons, from_pad.axes[0], from_keyboard == from_pad ? "одинаковы" : "РАЗНЫЕ");

    // Симуляция на обеих командах приходит к одному состоянию.
    Sim a, b;
    InputCommand idle;
    InputCommand last_a = idle, last_b = idle;
    for (int tick = 0; tick < 3; ++tick) { // команда зажата три тика: заклинание сработает один раз, по фронту
        a.step(last_a, from_keyboard);
        b.step(last_b, from_pad);
        last_a = from_keyboard;
        last_b = from_pad;
    }
    std::printf("после 3 тиков: x = %d, заклинаний %d (оба игрока)\n", a.x, a.casts);
    return from_keyboard == from_pad && a.x == b.x && a.casts == 1 && b.casts == 1 && a.x == 30 ? 0 : 1;
}
