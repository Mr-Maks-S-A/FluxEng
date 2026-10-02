#pragma once
/**
 * @file MemorySystem.hpp
 * @brief Общий заголовок модуля памяти FluxEng.
 *
 * | Что | Когда |
 * |---|---|
 * | MemorySystem::Arena | данные с общим временем жизни: уровень, кадр, запрос |
 * | MemorySystem::ArenaScope | временная память внутри функции |
 * | MemorySystem::DoubleArena | данные, которые живут один тик после создания |
 * | MemorySystem::Pool | объекты одного типа, создаются и умирают по одному |
 * | MemorySystem::ArenaResource | `std::pmr`-контейнеры внутри арены |
 * | MemorySystem::VirtualRegion | свой аллокатор поверх резерва ОС |
 */

#include <MemorySystem/Arena.hpp>
#include <MemorySystem/ArenaResource.hpp>
#include <MemorySystem/Core.hpp>
#include <MemorySystem/Pool.hpp>
#include <MemorySystem/Tags.hpp>
#include <MemorySystem/VirtualMemory.hpp>
