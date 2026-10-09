/**
 * @file 01_actions.cpp
 * @brief Действия вместо клавиш: ActionMap, клавиатура и геймпад в одной оси, переназначение, сохранение в текст.
 *
 * Игра спрашивает «прыжок?» и «куда идём?», а не «нажата ли W?». Игрок меняет управление, не трогая код игры.
 */

#include <InputSystem/InputSystem.hpp>

#include <cstdio>
#include <string>

using namespace InputSystem;

namespace {

struct Player {
    float x = 0.0f;
    int jumps = 0;
};

void frame(Player& player, const ActionMap& actions, const InputState& input, int number) {
    if (actions.pressed("jump", input)) ++player.jumps;
    player.x += actions.value("move_x", input) * 2.0f;
    std::printf("кадр %d: x = %5.2f, прыжков %d\n", number, static_cast<double>(player.x), player.jumps);
}

} // namespace

int main() {
    ActionMap actions;
    actions.bind("jump", Key::Space).bind("jump", GamepadButton::A);
    actions.bind_keys("move_x", Key::A, Key::D).bind_axis("move_x", GamepadAxis::LeftX);

    std::printf("--- привязки как текст (так они лежат в файле настроек):\n%s", actions.to_text().c_str());

    InputState input;
    Player player;
    // Кадр 0: идём вправо клавишей D. Кадр 1: прыжок пробелом. Кадр 2: подключился геймпад, стик влево и прыжок кнопкой A.
    input.begin_frame();
    input.apply(KeyInput{Key::D, Transition::Press, Modifiers::None});
    frame(player, actions, input, 0);

    input.begin_frame();
    input.apply(KeyInput{Key::D, Transition::Release, Modifiers::None});
    input.apply(KeyInput{Key::Space, Transition::Press, Modifiers::None});
    frame(player, actions, input, 1);

    input.begin_frame();
    input.apply(KeyInput{Key::Space, Transition::Release, Modifiers::None});
    input.apply(GamepadConnectionInput{0, true});
    input.apply(GamepadAxisInput{0, GamepadAxis::LeftX, -0.8f});
    input.apply(GamepadButtonInput{0, GamepadButton::A, Transition::Press});
    frame(player, actions, input, 2);

    // Игрок переназначает прыжок на J — одной строкой текста; код игры не меняется.
    std::printf("--- переназначение: jump → J\n");
    const auto user_settings = ActionMap::from_text("jump: Key:J\nmove_x: Keys:Left,Right\n");
    if (!user_settings) {
        std::printf("ошибка настроек: %s\n", user_settings.error().c_str());
        return 1;
    }
    input.begin_frame();
    input.apply(KeyInput{Key::Space, Transition::Press, Modifiers::None}); // старая клавиша больше не прыжок
    frame(player, *user_settings, input, 3);
    input.begin_frame();
    input.apply(KeyInput{Key::J, Transition::Press, Modifiers::None});
    frame(player, *user_settings, input, 4);

    // Ожидаем: прыжков ровно 3 (пробел, A на геймпаде, J), игрок не вернулся в ноль.
    return player.jumps == 3 ? 0 : 1;
}
