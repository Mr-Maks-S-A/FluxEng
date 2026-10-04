#pragma once
/**
 * @file Referee.hpp
 * @brief Судья: следит за симуляцией после каждого тика и решает, выиграна ли цель, нарушены ли пределы, сколько звёзд.
 *
 * Судья **только читает** мир (`const Simulation&`) и сам не состояние симуляции: хеши, записи и сеть его не касаются.
 * Из-за этого он везде даёт один результат — в игре, в повторе и у каждого сетевого игрока: вердикт считается из того же
 * детерминированного мира. После победы или поражения судья замирает: метрики не меняются, результат фиксирован.
 *
 * ```
 *   Referee ref(level);
 *   каждый тик:  sim.tick(commands);  ref.observe(sim);
 *   ref.status() — Running / Won / Lost;  ref.metrics();  ref.stars();  ref.status_line() — строка для HUD
 * ```
 */

#include <Challenge/Level.hpp>

namespace Challenge {

enum class Status : std::uint8_t { Running, Won, Lost };
enum class Loss : std::uint8_t { None, OutOfTime, TooManyCasts, TooMuchMana, ProgramTooLong };

struct Metrics {
    std::uint32_t ticks = 0;
    std::uint32_t casts = 0;
    std::uint32_t failed_casts = 0;
    std::int32_t mana_spent = 0;        ///< Суммарные траты личной маны (падения запаса; восстановление не вычитается).
    std::uint32_t program_runes = 0;    ///< Самая длинная программа среди слотов гримуара.
    std::uint32_t programs_set = 0;     ///< Принятых правок `SetProgram`.
};

class Referee {
public:
    explicit Referee(const Level& level) : m_level(&level) {}

    /// @brief Вызывать после каждого тика симуляции (до окончания игры; потом — без эффекта).
    void observe(const SpellSim::Simulation& sim);

    [[nodiscard]] Status status() const noexcept { return m_status; }
    [[nodiscard]] Loss loss() const noexcept { return m_loss; }
    [[nodiscard]] const Metrics& metrics() const noexcept { return m_metrics; }
    [[nodiscard]] const Level& level() const noexcept { return *m_level; }

    /// @brief Звёзды 0…3: 0 — не выиграно; ★ победа; ★★ ещё и мана в планке; ★★★ ещё и программа в планке.
    [[nodiscard]] int stars() const noexcept;
    /// @brief Расстояние до цели в метрах (Reach — до зоны; Carve/Raise — до точки): для стрелки-подсказки и сортировки попыток.
    [[nodiscard]] Math::Fixed distance() const noexcept { return m_distance; }

    /// @brief Строка состояния для HUD: цель, время, мана, касты, исход.
    [[nodiscard]] std::string status_line() const;
    [[nodiscard]] static std::string_view loss_text(Loss loss) noexcept;

private:
    void judge(const SpellSim::Simulation& sim);

    const Level* m_level;
    Status m_status = Status::Running;
    Loss m_loss = Loss::None;
    Metrics m_metrics;
    Math::Fixed m_distance{};
    Math::Mana m_last_mana{};
    std::int64_t m_spent_raw = 0;
    bool m_started = false;
};

} // namespace Challenge
