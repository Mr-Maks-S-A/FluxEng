#pragma once
/**
 * @file Command.hpp
 * @brief Команда ввода за тик: плоская структура целых чисел, которую можно отправить по сети и воспроизвести.
 *
 * Мост между «живым» вводом (клавиши, мышь, стик) и детерминированной симуляцией:
 * ```
 * InputState + ActionMap ──sample()──► InputCommand (16 байт) ──► сеть / запись ──► симуляция на всех узлах
 * ```
 * Симуляция читает только команду — ей всё равно, клавиатура это, геймпад или бот. Структура без padding и без float
 * (`has_unique_object_representations`), поэтому подходит как команда NetSystem::Lockstep: одинаковые байты на всех платформах.
 *
 * Состав команды задаёт CommandLayout: какие действия — кнопки (до 32), какие — оси (до 6). Раскладка одинакова у всех узлов
 * и зашита в игру; в сеть уходят только числа.
 */

#include <InputSystem/ActionMap.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace InputSystem {

struct InputCommand {
    static constexpr std::size_t kMaxButtons = 32;
    static constexpr std::size_t kMaxAxes = 6;
    static constexpr std::int16_t kAxisMax = 32767;

    std::uint32_t buttons = 0;                  ///< Бит i — кнопка i зажата.
    std::int16_t axes[kMaxAxes] = {};           ///< Ось j: −32767…+32767 = −1…+1.

    [[nodiscard]] bool down(std::size_t button) const noexcept { return button < kMaxButtons && ((buttons >> button) & 1u) != 0; }
    /// @brief Ось в долях единицы (для визуализации; симуляции лучше работать с целыми).
    [[nodiscard]] float axis(std::size_t index) const noexcept {
        return index < kMaxAxes ? static_cast<float>(axes[index]) / static_cast<float>(kAxisMax) : 0.0f;
    }

    friend bool operator==(const InputCommand&, const InputCommand&) noexcept = default;
};
static_assert(sizeof(InputCommand) == 16 && std::has_unique_object_representations_v<InputCommand> &&
              std::is_trivially_copyable_v<InputCommand>);

/// @brief Кнопка нажата в `current`, но не была зажата в `previous` (фронт восстанавливается из двух подряд идущих команд).
[[nodiscard]] constexpr bool button_pressed(const InputCommand& previous, const InputCommand& current, std::size_t button) noexcept {
    return current.down(button) && !previous.down(button);
}
[[nodiscard]] constexpr bool button_released(const InputCommand& previous, const InputCommand& current, std::size_t button) noexcept {
    return !current.down(button) && previous.down(button);
}

/// @brief Какие действия попадают в команду и в каком порядке.
class CommandLayout {
public:
    /// @brief Следующая кнопка (индекс = порядок добавления).
    CommandLayout& button(std::string_view action) {
        if (m_buttons.size() >= InputCommand::kMaxButtons) throw std::length_error("CommandLayout: more than 32 buttons");
        m_buttons.push_back(action_id(action));
        return *this;
    }
    /// @brief Следующая ось.
    CommandLayout& axis(std::string_view action) {
        if (m_axes.size() >= InputCommand::kMaxAxes) throw std::length_error("CommandLayout: more than 6 axes");
        m_axes.push_back(action_id(action));
        return *this;
    }
    [[nodiscard]] const std::vector<ActionId>& buttons() const noexcept { return m_buttons; }
    [[nodiscard]] const std::vector<ActionId>& axes() const noexcept { return m_axes; }

private:
    std::vector<ActionId> m_buttons;
    std::vector<ActionId> m_axes;
};

/// @brief Снимок ввода на тик: значения действий раскладки → числа. Вызывается на узле, снимающем ввод (клиент).
[[nodiscard]] inline InputCommand sample_command(const ActionMap& actions, const InputState& state, const CommandLayout& layout) noexcept {
    InputCommand command;
    for (std::size_t i = 0; i < layout.buttons().size(); ++i) {
        if (actions.down(layout.buttons()[i], state)) command.buttons |= 1u << i;
    }
    for (std::size_t j = 0; j < layout.axes().size(); ++j) {
        const float v = std::fmax(-1.0f, std::fmin(1.0f, actions.value(layout.axes()[j], state)));
        command.axes[j] = static_cast<std::int16_t>(std::lround(v * static_cast<float>(InputCommand::kAxisMax)));
    }
    return command;
}

} // namespace InputSystem
