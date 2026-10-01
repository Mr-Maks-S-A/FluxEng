#pragma once
/**
 * @file JobSystem.hpp
 * @brief Общий заголовок JobSystem: `#include <JobSystem/JobSystem.hpp>`.
 *
 * | Что | Где |
 * |---|---|
 * | JobSystem::Scheduler — пул потоков, run / wait, scratch-арены | Scheduler.hpp |
 * | JobSystem::JobCounter, JobSystem::TaskGroup — ожидание групп задач | Scheduler.hpp |
 * | JobSystem::parallel_for, JobSystem::parallel_reduce — детерминированная нарезка | Parallel.hpp |
 * | JobSystem::ChunkBuffers — выходы кусков, слияние по порядку | Parallel.hpp |
 */

#include <JobSystem/Parallel.hpp>
#include <JobSystem/Scheduler.hpp>
