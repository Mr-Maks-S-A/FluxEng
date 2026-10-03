/**
 * @example 01_actions.cpp
 * Ввод через действия: игра объявляет «прыжок», «вперёд» и спрашивает про них, не зная клавиш.
 * Привязки меняются файлом игрока; несколько клавиш на действие; оси; проверка конфликтов.
 *
 * Пример не открывает окно: состояние ввода (`InputState`) заполняется вручную — так же пишутся тесты игр.
 */

#include <Core/Core.hpp>

#include <GLFW/glfw3.h>

#include <cstdio>

using Core::ActionMap;
using Core::Binding;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    // 1. Объявляем действия по смыслу и привязываем клавиши по умолчанию.
    ActionMap actions;
    const auto jump = actions.declare("jump", "Прыжок");
    const auto forward = actions.declare("move_forward", "Вперёд");
    const auto back = actions.declare("move_back", "Назад");
    const auto fire = actions.declare("fire", "Каст из личного запаса");
    actions.bind(jump, Binding::key(GLFW_KEY_SPACE));
    actions.bind(forward, Binding::key(GLFW_KEY_W)).bind(forward, Binding::key(GLFW_KEY_UP)); // две клавиши на одно действие
    actions.bind(back, Binding::key(GLFW_KEY_S));
    actions.bind(fire, Binding::mouse(GLFW_MOUSE_BUTTON_LEFT));

    // 2. Каждый кадр игра спрашивает про действия. Состояние ввода заполняет окно; здесь — вручную.
    WindowSystem::InputState input;
    input.on_key(GLFW_KEY_UP, WindowSystem::action_press); // игрок нажал стрелку вверх
    std::printf("1. «вперёд» зажато: %s, нажато в этом кадре: %s, «прыжок»: %s\n", actions.down(input, forward) ? "да" : "нет",
                actions.pressed(input, forward) ? "да" : "нет", actions.down(input, jump) ? "да" : "нет");
    EXPECT(actions.down(input, forward) && actions.pressed(input, forward) && !actions.down(input, jump));
    std::printf("   ось вперёд/назад = %+.0f\n", actions.axis(input, forward, back));
    EXPECT(actions.axis(input, forward, back) == 1.0f);

    input.begin_frame(); // новый кадр: «нажато в этом кадре» сброшено, «зажато» осталось
    EXPECT(actions.down(input, forward) && !actions.pressed(input, forward));

    // 3. Файл игрока переопределяет привязки только упомянутых действий; ошибка в файле — ничего не применяется.
    const auto applied = actions.apply("# мой файл\njump = F, MOUSE_RIGHT\nmove_back = X\n");
    EXPECT(applied.has_value());
    input.on_mouse_button(GLFW_MOUSE_BUTTON_RIGHT, WindowSystem::action_press);
    std::printf("2. после перепривязки «прыжок» срабатывает от правой кнопки мыши: %s\n", actions.pressed(input, jump) ? "да" : "нет");
    EXPECT(actions.pressed(input, jump));
    const auto bad = actions.apply("jump = F\nfly = X\n");
    EXPECT(!bad.has_value());
    std::printf("3. файл с ошибкой отвергнут целиком: %s\n", bad.error().c_str());

    // 4. Конфликты: два действия на одной клавише — игра может предупредить игрока при перепривязке.
    actions.bind(fire, Binding::key(GLFW_KEY_X)); // X уже у «назад»
    const auto clash = actions.conflicts(fire);
    std::printf("4. конфликт «fire» с: %s\n", clash.empty() ? "-" : actions.name(clash.front()).c_str());
    EXPECT(clash.size() == 1 && clash.front() == back);

    // 5. Файл настроек готов: serialize() выдаёт его целиком (с описаниями) — это то, что читает apply().
    std::printf("5. текущие привязки:\n%s", actions.serialize().c_str());
    std::printf("OK\n");
    return 0;
}
