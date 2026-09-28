#pragma once
/**
 * @file EventSystem.hpp
 * @brief Общий заголовок модуля: подключает весь публичный API системы событий.
 */

#include <EventSystem/Core/Error.hpp>
#include <EventSystem/Core/Event.hpp>
#include <EventSystem/Core/FNV1a.hpp>
#include <EventSystem/Core/FixedString.hpp>
#include <EventSystem/Core/Ids.hpp>
#include <EventSystem/Core/Schema.hpp>

#include <EventSystem/Storage/ColumnBuffer.hpp>
#include <EventSystem/Storage/EventBuffer.hpp>

#include <EventSystem/Channel/IChannel.hpp>
#include <EventSystem/Channel/StreamChannel.hpp>

#include <EventSystem/Bus/EventBus.hpp>
#include <EventSystem/Bus/EventReader.hpp>
#include <EventSystem/Bus/EventWriter.hpp>

#include <EventSystem/Graph/EventGraph.hpp>
#include <EventSystem/Graph/ModuleRegistry.hpp>

/**
 * @namespace EventSystem
 * @brief Система событий FluxEng: data-oriented шина, связывающая модули движка.
 */
