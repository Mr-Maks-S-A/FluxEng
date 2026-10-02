#pragma once
/**
 * @file RuntimeSystem.hpp
 * @brief Общий заголовок RuntimeSystem: `#include <RuntimeSystem/RuntimeSystem.hpp>`.
 *
 * | Что | Где |
 * |---|---|
 * | RuntimeSystem::Runtime — шина, задачи, память, тик, модули; цикл без окна | Runtime.hpp |
 * | RuntimeSystem::Module — жизненный цикл модуля и зависимости | Module.hpp |
 * | RuntimeSystem::FixedStep — сколько тиков выполнить за кадр | FixedStep.hpp |
 */

#include <RuntimeSystem/FixedStep.hpp>
#include <RuntimeSystem/Module.hpp>
#include <RuntimeSystem/Runtime.hpp>
