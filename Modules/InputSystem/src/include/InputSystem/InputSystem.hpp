#pragma once
/**
 * @file InputSystem.hpp
 * @brief Общий заголовок InputSystem: `#include <InputSystem/InputSystem.hpp>`.
 *
 * | Что | Где |
 * |---|---|
 * | InputSystem::Key, MouseButton, GamepadButton, GamepadAxis, Transition, Modifiers — собственные стабильные коды | Keys.hpp |
 * | InputSystem::InputEvent и события *Input — ввод от платформы | Events.hpp |
 * | InputSystem::InputState — что зажато/нажато/отпущено, курсор, колесо, текст, геймпады | InputState.hpp |
 * | InputSystem::ActionMap — действия игры ↔ клавиши/кнопки/оси; переназначение, сохранение в текст | ActionMap.hpp |
 * | InputSystem::InputCommand, CommandLayout, sample_command — команда ввода за тик для сети и реплеев | Command.hpp |
 * | InputSystem::InputLog — запись и воспроизведение ввода | InputLog.hpp |
 */

#include <InputSystem/ActionMap.hpp>
#include <InputSystem/Command.hpp>
#include <InputSystem/Events.hpp>
#include <InputSystem/InputLog.hpp>
#include <InputSystem/InputState.hpp>
#include <InputSystem/Keys.hpp>
