#pragma once
/**
 * @file PlatformEvents.hpp
 * @brief События платформы (ввод), которые каркас Sandbox отправляет в шину от имени модуля "Platform".
 */

#include <EventSystem/EventSystem.hpp>

#include <cstdint>
#include <string_view>

namespace Sandbox {

/// @brief Клавиша нажата / отпущена / повторена (коды и действия GLFW).
struct KeyEvent {
    std::int32_t key = 0;    ///< GLFW_KEY_*.
    std::int32_t action = 0; ///< GLFW_PRESS / GLFW_RELEASE / GLFW_REPEAT.

    static constexpr std::string_view event_name = "platform.key";
    using fields = EventSystem::Fields<
        EventSystem::Field<"key", &KeyEvent::key>,
        EventSystem::Field<"action", &KeyEvent::action>>;
};

/// @brief Кнопка мыши нажата / отпущена; координаты уже переведены в мир камерой.
struct MouseButtonEvent {
    std::int32_t button = 0; ///< GLFW_MOUSE_BUTTON_*.
    std::int32_t action = 0; ///< GLFW_PRESS / GLFW_RELEASE.
    float world_x = 0.0f;    ///< Точка мира под курсором.
    float world_y = 0.0f;

    static constexpr std::string_view event_name = "platform.mouse_button";
    using fields = EventSystem::Fields<
        EventSystem::Field<"button", &MouseButtonEvent::button>,
        EventSystem::Field<"action", &MouseButtonEvent::action>,
        EventSystem::Field<"world_x", &MouseButtonEvent::world_x>,
        EventSystem::Field<"world_y", &MouseButtonEvent::world_y>>;
};

} // namespace Sandbox
