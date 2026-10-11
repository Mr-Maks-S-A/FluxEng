#pragma once
/**
 * @file NetSystem.hpp
 * @brief Общий заголовок NetSystem: `#include <NetSystem/NetSystem.hpp>`.
 *
 * | Что | Где |
 * |---|---|
 * | NetSystem::ByteWriter, ByteReader — сериализация (little-endian, чтение с проверкой границ) | Bytes.hpp |
 * | NetSystem::ITransport, Packet, PeerId — датаграммы между узлами | Transport.hpp |
 * | NetSystem::SimulatedNetwork — сеть в процессе: задержка, джиттер, потери, дубликаты, виртуальное время | SimulatedNetwork.hpp |
 * | NetSystem::Lockstep, LockstepNode — детерминированный lockstep, звезда и mesh, проверка хешей состояния | Lockstep.hpp |
 */

#include <NetSystem/Bytes.hpp>
#include <NetSystem/Lockstep.hpp>
#include <NetSystem/SimulatedNetwork.hpp>
#include <NetSystem/Transport.hpp>
