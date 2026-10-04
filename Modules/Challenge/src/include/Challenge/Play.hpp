#pragma once
/**
 * @file Play.hpp
 * @brief Безголовый прогон уровня: решение (программы + сценарий ввода) → исход. Для тестов, бенчмарков и «живых» проверок.
 *
 * Тот же путь, что у игры: программы уходят блобами и командами `SetProgram` через `Replay::Driver`, поэтому прогон можно
 * записать (`Replay::Session::record`) и воспроизвести тем же вызовом.
 */

#include <Challenge/Referee.hpp>

namespace Challenge {

struct Outcome {
    Status status = Status::Running;
    Loss loss = Loss::None;
    Metrics metrics;
    int stars = 0;
    Math::Fixed distance{};       ///< Расстояние до цели на момент исхода (метры).
    std::uint32_t ticks_run = 0;
    Replay::StateHashes hashes;
    std::string status_line;
};

/**
 * @brief Прогоняет `solution` на `level` до исхода или `max_ticks` уровня.
 * @param session запись/повтор (по умолчанию без записи). При повторе сценарий берётся из записи, `solution.steps` игнорируется.
 */
[[nodiscard]] Outcome play(const Level& level, const Solution& solution, Replay::Session* session = nullptr);

/// @brief «Ничего не делать»: игрок стоит на месте до конца времени. Нужно уровням, требующим заклинаний, как контрольный проигрыш.
[[nodiscard]] Outcome play_idle(const Level& level);

} // namespace Challenge
