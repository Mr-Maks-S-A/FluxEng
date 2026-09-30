#pragma once
/**
 * @file ECSSystem.hpp
 * @brief Общий заголовок ECS FluxEng: `#include <ECSSystem/ECSSystem.hpp>`.
 *
 * | Что | Где |
 * |---|---|
 * | ECS::Entity — ссылка с поколением, `Entity{}` = «нет сущности» | Entity.hpp |
 * | ECS::EntityRegistry — выдача слотов и поколения | Entity.hpp |
 * | ECS::ComponentPool — sparse set одного типа | ComponentPool.hpp |
 * | ECS::View — выборка по нескольким компонентам | View.hpp |
 * | ECS::World — всё вместе | World.hpp |
 */

#include <ECSSystem/ComponentPool.hpp>
#include <ECSSystem/Entity.hpp>
#include <ECSSystem/View.hpp>
#include <ECSSystem/World.hpp>
