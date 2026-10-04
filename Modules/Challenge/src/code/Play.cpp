#include <Challenge/Play.hpp>

namespace Challenge {

namespace {

Outcome run(const Level& level, const Solution* solution, Replay::Session* external) {
    SpellSim::Simulation sim(level.config);
    install_library(level, sim);
    Replay::Session off = Replay::Session::off(level.config.seed);
    Replay::Session& session = external != nullptr ? *external : off;
    Replay::Driver driver(sim, session);
    Referee referee(level);

    // Программы решения: блобы и команды SetProgram на тике 0.
    std::vector<Replay::Command> opening;
    if (solution != nullptr && !driver.replaying()) {
        for (const auto& [slot, text] : solution->programs) {
            const auto program = Runes::parse_program(text);
            if (!program) continue; // решение с ошибкой в тексте: слот останется прежним, судья покажет проигрыш
            opening.push_back(SpellSim::set_program_command(slot, driver.submit_blob(Runes::encode_program(*program))));
        }
    }

    const std::uint32_t limit = level.limits.max_ticks != 0 ? level.limits.max_ticks : 60u * 60u;
    std::size_t next = 0;
    std::vector<Replay::Command> live;
    while (referee.status() == Status::Running && sim.tick_number() < limit + 1) {
        const std::uint32_t tick = sim.tick_number();
        live.clear();
        if (tick == 0) live = opening;
        if (solution != nullptr) {
            while (next < solution->steps.size() && solution->steps[next].tick <= tick) {
                if (solution->steps[next].tick == tick) live.push_back(solution->steps[next].command);
                ++next;
            }
        }
        if (solution != nullptr && solution->policy && !driver.replaying()) solution->policy(sim, live);
        if (!driver.step(live)) break;
        referee.observe(sim);
    }

    Outcome out;
    out.status = referee.status();
    out.loss = referee.loss();
    out.metrics = referee.metrics();
    out.stars = referee.stars();
    out.distance = referee.distance();
    out.ticks_run = sim.tick_number();
    out.hashes = sim.hashes();
    out.status_line = referee.status_line();
    return out;
}

} // namespace

Outcome play(const Level& level, const Solution& solution, Replay::Session* session) { return run(level, &solution, session); }
Outcome play_idle(const Level& level) { return run(level, nullptr, nullptr); }

} // namespace Challenge
