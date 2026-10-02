#pragma once
/**
 * @file FixedStep.hpp
 * @brief Совместимость: FixedStep переехал в RuntimeSystem (ядро без графики и окна).
 */

#include <RuntimeSystem/FixedStep.hpp>

namespace Core {

using FixedStep = RuntimeSystem::FixedStep;

} // namespace Core
