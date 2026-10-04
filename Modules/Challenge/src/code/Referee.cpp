#include <Challenge/Referee.hpp>

#include <cstdio>

namespace Challenge {

namespace {

/// Расстояние между точками мира в Fixed-метрах (целочисленно: корень суммы квадратов).
Math::Fixed distance_between(Math::WorldPos a, Math::WorldPos b) noexcept {
    const std::int64_t dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    const std::uint64_t root = Math::isqrt(static_cast<std::uint64_t>(dx * dx + dy * dy + dz * dz));
    return Math::Fixed::saturate(static_cast<std::int64_t>(root));
}

} // namespace

void Referee::observe(const SpellSim::Simulation& sim) {
    if (m_status != Status::Running) return;
    const Goal& goal = m_level->goal;
    const Character::ManaPool& pool = *sim.world().get<Character::ManaPool>(sim.player());
    if (m_started && pool.current < m_last_mana) m_spent_raw += m_last_mana.raw() - pool.current.raw(); // сумма падений: дробные доли не теряются
    m_metrics.mana_spent = static_cast<std::int32_t>(m_spent_raw / Math::Fixed::one_raw);
    m_last_mana = pool.current;
    m_started = true;

    m_metrics.ticks = sim.tick_number();
    m_metrics.casts = sim.casts();
    m_metrics.failed_casts = sim.failed_casts();
    m_metrics.programs_set = sim.programs_set();
    const Character::Grimoire& grimoire = *sim.world().get<Character::Grimoire>(sim.player());
    std::uint32_t longest = 0;
    for (const std::string& name : grimoire.slots) {
        if (const auto program = sim.programs().find(name)) longest = std::max(longest, static_cast<std::uint32_t>(program->code.size()));
    }
    m_metrics.program_runes = longest;

    // Цель.
    bool reached = false;
    switch (goal.kind) {
    case GoalKind::Reach: {
        const Math::WorldPos feet = sim.world().get<Character::Position>(sim.player())->value;
        m_distance = distance_between(feet, goal.point);
        reached = m_distance <= goal.radius;
        break;
    }
    case GoalKind::Carve:
    case GoalKind::Raise: {
        const Math::Fixed d = sim.terrain().sample(goal.point); // отрицательно — порода, положительно — пустота
        reached = goal.kind == GoalKind::Carve ? d > Math::Fixed{} : d < Math::Fixed{};
        const Math::Fixed shortfall = goal.kind == GoalKind::Carve ? -d : d;
        m_distance = reached ? Math::Fixed{} : Math::max(shortfall, Math::Fixed{});
        break;
    }
    }

    // Пределы проверяются раньше победы: выиграл, нарушив предел на том же тике, — не выиграл.
    const Limits& lim = m_level->limits;
    Loss loss = Loss::None;
    if (lim.max_casts != 0 && m_metrics.casts > lim.max_casts) loss = Loss::TooManyCasts;
    else if (lim.max_mana_spent != 0 && m_metrics.mana_spent > lim.max_mana_spent) loss = Loss::TooMuchMana;
    else if (lim.max_program_runes != 0 && m_metrics.program_runes > lim.max_program_runes) loss = Loss::ProgramTooLong;
    if (loss != Loss::None) { m_status = Status::Lost; m_loss = loss; return; }
    if (reached) { m_status = Status::Won; return; }
    if (lim.max_ticks != 0 && m_metrics.ticks >= lim.max_ticks) { m_status = Status::Lost; m_loss = Loss::OutOfTime; }
}

int Referee::stars() const noexcept {
    if (m_status != Status::Won) return 0;
    int stars = 1;
    const Par& par = m_level->par;
    const bool mana_ok = par.mana_spent == 0 || m_metrics.mana_spent <= par.mana_spent;
    const bool runes_ok = par.program_runes == 0 || m_metrics.program_runes <= par.program_runes;
    if (mana_ok) ++stars;
    if (mana_ok && runes_ok) ++stars;
    return stars;
}

std::string_view Referee::loss_text(Loss loss) noexcept {
    switch (loss) {
    case Loss::None: return "";
    case Loss::OutOfTime: return "время вышло";
    case Loss::TooManyCasts: return "слишком много кастов";
    case Loss::TooMuchMana: return "потрачено слишком много маны";
    case Loss::ProgramTooLong: return "программа слишком длинная";
    }
    return "";
}

std::string Referee::status_line() const {
    char buffer[200];
    const Limits& lim = m_level->limits;
    const double seconds = static_cast<double>(m_metrics.ticks) / SpellSim::ticks_per_second;
    int n = std::snprintf(buffer, sizeof buffer, "%s · %.1f", m_level->title.c_str(), seconds);
    if (lim.max_ticks != 0) n += std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), "/%u с", lim.max_ticks / SpellSim::ticks_per_second);
    else n += std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), " с");
    n += std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), " · мана %d", m_metrics.mana_spent);
    if (lim.max_mana_spent != 0) n += std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), "/%d", lim.max_mana_spent);
    n += std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), " · касты %u", m_metrics.casts);
    if (lim.max_casts != 0) n += std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), "/%u", lim.max_casts);
    n += std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), " · рун %u", m_metrics.program_runes);
    if (lim.max_program_runes != 0) n += std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), "/%u", lim.max_program_runes);
    if (m_status == Status::Running) std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), " · до цели %.1f м", m_distance.to_double());
    else if (m_status == Status::Won) std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), " · ПОБЕДА %s", std::string(stars() == 3 ? "***" : stars() == 2 ? "**" : "*").c_str());
    else std::snprintf(buffer + n, sizeof buffer - static_cast<std::size_t>(n), " · ПОРАЖЕНИЕ: %s", std::string(loss_text(m_loss)).c_str());
    return buffer;
}

} // namespace Challenge
