#include <Phases/Schedule.hpp>

#include <Math/Assert.hpp>

#include <chrono>

namespace Phases {

std::size_t Schedule::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < m_phases.size(); ++i) {
        if (m_phases[i].name == name) return i;
    }
    return npos;
}

void Schedule::rebuild_times() {
    m_times.clear();
    m_times.reserve(m_phases.size());
    for (const Phase& p : m_phases) m_times.push_back({p.name, 0.0}); // string_view на имена внутри m_phases
}

Schedule& Schedule::add(std::string name, Fn fn) {
    FLUX_ASSERT(find(name) == npos, "Schedule::add: имя фазы уже занято");
    m_phases.push_back({std::move(name), std::move(fn), true});
    rebuild_times();
    return *this;
}

bool Schedule::insert_before(std::string_view anchor, std::string name, Fn fn) {
    const std::size_t at = find(anchor);
    if (at == npos) return false;
    FLUX_ASSERT(find(name) == npos, "Schedule::insert_before: имя фазы уже занято");
    m_phases.insert(m_phases.begin() + static_cast<std::ptrdiff_t>(at), {std::move(name), std::move(fn), true});
    rebuild_times();
    return true;
}

bool Schedule::insert_after(std::string_view anchor, std::string name, Fn fn) {
    const std::size_t at = find(anchor);
    if (at == npos) return false;
    FLUX_ASSERT(find(name) == npos, "Schedule::insert_after: имя фазы уже занято");
    m_phases.insert(m_phases.begin() + static_cast<std::ptrdiff_t>(at) + 1, {std::move(name), std::move(fn), true});
    rebuild_times();
    return true;
}

bool Schedule::remove(std::string_view name) {
    const std::size_t at = find(name);
    if (at == npos) return false;
    m_phases.erase(m_phases.begin() + static_cast<std::ptrdiff_t>(at));
    rebuild_times();
    return true;
}

bool Schedule::set_enabled(std::string_view name, bool enabled) {
    const std::size_t at = find(name);
    if (at == npos) return false;
    m_phases[at].enabled = enabled;
    return true;
}

std::vector<std::string_view> Schedule::names() const {
    std::vector<std::string_view> out;
    out.reserve(m_phases.size());
    for (const Phase& p : m_phases) out.push_back(p.name);
    return out;
}

void Schedule::run() {
    using Clock = std::chrono::steady_clock;
    m_total = 0.0;
    for (std::size_t i = 0; i < m_phases.size(); ++i) {
        Phase& phase = m_phases[i];
        if (!phase.enabled) {
            m_times[i].milliseconds = 0.0;
            continue;
        }
        const auto start = Clock::now();
        phase.fn();
        m_times[i].milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        m_total += m_times[i].milliseconds;
    }
}

} // namespace Phases
