#pragma once
/**
 * @file PlatformEvents.hpp
 * @brief События платформы (ввод), которые слой приложения (Core::App) отправляет в шину от имени модуля "Platform".
 */

#include <EventSystem/EventSystem.hpp>
#include <InputSystem/Keys.hpp>

#include <cstdint>
#include <string_view>

namespace Core {

/// @brief Клавиша нажата / отпущена / повторена. Коды — собственные коды движка (InputSystem::Key, InputSystem::Transition):
/// в шине, записях и реплеях лежат стабильные числа, а не значения какой-либо библиотеки окон.
struct KeyEvent {
    std::int32_t key = 0;    ///< InputSystem::Key.
    std::int32_t action = 0; ///< InputSystem::Transition.

    [[nodiscard]] InputSystem::Key code() const noexcept { return static_cast<InputSystem::Key>(key); }
    [[nodiscard]] InputSystem::Transition transition() const noexcept { return static_cast<InputSystem::Transition>(action); }
    [[nodiscard]] bool pressed() const noexcept { return transition() == InputSystem::Transition::Press; }
    [[nodiscard]] bool released() const noexcept { return transition() == InputSystem::Transition::Release; }

    static constexpr std::string_view event_name = "platform.key";
    using fields = EventSystem::Fields<
        EventSystem::Field<"key", &KeyEvent::key>,
        EventSystem::Field<"action", &KeyEvent::action>>;
};

/// @brief Кнопка мыши нажата / отпущена; координаты уже переведены в мир камерой.
struct MouseButtonEvent {
    std::int32_t button = 0; ///< InputSystem::MouseButton.
    std::int32_t action = 0; ///< InputSystem::Transition (Press / Release).
    float world_x = 0.0f;    ///< Точка мира под курсором.
    float world_y = 0.0f;

    [[nodiscard]] InputSystem::MouseButton which() const noexcept { return static_cast<InputSystem::MouseButton>(button); }
    [[nodiscard]] bool pressed() const noexcept { return action == static_cast<std::int32_t>(InputSystem::Transition::Press); }
    [[nodiscard]] bool released() const noexcept { return action == static_cast<std::int32_t>(InputSystem::Transition::Release); }

    static constexpr std::string_view event_name = "platform.mouse_button";
    using fields = EventSystem::Fields<
        EventSystem::Field<"button", &MouseButtonEvent::button>,
        EventSystem::Field<"action", &MouseButtonEvent::action>,
        EventSystem::Field<"world_x", &MouseButtonEvent::world_x>,
        EventSystem::Field<"world_y", &MouseButtonEvent::world_y>>;
};

} // namespace Core
