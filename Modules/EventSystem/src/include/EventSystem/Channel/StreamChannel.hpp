#pragma once
/**
 * @file StreamChannel.hpp
 * @brief Совместимость: раньше канал Stream был отдельным классом; теперь все политики — EventSystem::Channel.
 */

#include <EventSystem/Channel/Channel.hpp>

namespace EventSystem {

/// @brief Прежнее имя канала (политика по умолчанию — Delivery::Stream).
using StreamChannel = Channel;

} // namespace EventSystem
