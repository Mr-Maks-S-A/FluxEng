/**
 * @file main.cpp
 * @brief CoopScribe — «Пилот и писарь»: два игрока проходят уровень вместе по (плохой) сети, без окна и графики.
 *
 * Роли делят одного мага: **пилот** ходит и кастует, **писарь** пишет заклинания (программа уходит блобом, в потоке команд — `SetProgram`).
 * У каждого свой экземпляр симуляции; по сети идёт только ввод (`Net::Lockstep`), а не состояние мира. В конце оба мира
 * обязаны совпасть побитно — хеши подсистем сравниваются по ходу и в итоге.
 *
 * Что показывает пример (всё — готовые модули, в игре только склейка):
 * - **Challenge** — уровень, судья, эталонное решение (программы → писарь, бот-прицел → пилот);
 * - **Net** — lockstep поверх `LoopbackNetwork` с потерями, задержкой, дублями и порчей;
 * - **SpellSim** — `SetProgram` в потоке команд, блобы по хешу;
 * - **Runes** — `encode_program`, хеш программы.
 *
 * Аргументы: `--level id` (walk | mine | mound | fort; по умолчанию mound — там писарь нужен), `--latency N` (шагов сети), `--loss N` (‰),
 * `--jitter N`, `--dup N` (‰), `--corrupt N` (‰), `--delay N` (задержка ввода, тиков), `--seed N` (сид сети),
 * `--cheat` (писарь «подправляет» свой мир мимо сети: расхождение должно быть обнаружено и названо по подсистеме).
 * Код возврата 0 — успех; в последней строке `OK`.
 */

#include <Challenge/Play.hpp>
#include <Net/Lockstep.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string level = "mound";
    Net::LinkConfig link{.latency_steps = 2, .jitter_steps = 3, .loss_permille = 150, .duplicate_permille = 50, .corrupt_permille = 20};
    std::uint32_t input_delay = 3;
    std::uint64_t net_seed = 1;
    bool cheat = false;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const auto has_value = [&] { return i + 1 < argc; };
        const std::string a = argv[i];
        if (a == "--level" && has_value()) o.level = argv[++i];
        else if (a == "--latency" && has_value()) o.link.latency_steps = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--loss" && has_value()) o.link.loss_permille = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--jitter" && has_value()) o.link.jitter_steps = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--dup" && has_value()) o.link.duplicate_permille = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--corrupt" && has_value()) o.link.corrupt_permille = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--delay" && has_value()) o.input_delay = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--seed" && has_value()) o.net_seed = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        else if (a == "--cheat") o.cheat = true;
    }
    return o;
}

/// Один участник: свой мир, судья и сетевой узел. Роль определяет, что игрок вносит в общий ввод.
class Player {
public:
    enum class Role { Pilot, Scribe };

    Player(Role role, const Challenge::Level& level, Net::Transport& transport, Net::PeerId id, std::uint32_t input_delay)
        : m_role(role), m_level(level), m_sim(level.config), m_referee(level),
          m_net(transport, {.local = id, .peers = 2, .seed = level.config.seed, .config_hash = Challenge::level_hash(level), .input_delay = input_delay}) {
        Challenge::install_library(level, m_sim);
        m_net.set_blob_reference(SpellSim::blob_reference); // SetProgram ждёт свою программу
    }

    [[nodiscard]] bool done() const { return m_referee.status() != Challenge::Status::Running; }
    [[nodiscard]] const Challenge::Referee& referee() const { return m_referee; }
    [[nodiscard]] const SpellSim::Simulation& sim() const { return m_sim; }
    [[nodiscard]] const Net::Lockstep& net() const { return m_net; }

    /// Кадр: сеть → ввод → шаг. Закончивший игрок продолжает `pump`: его пакеты (подтверждения, ввод) нужны партнёру.
    void frame(const Challenge::Solution& solution, bool cheat) {
        m_net.pump();
        if (done()) return;
        if (m_net.needs_input()) {
            std::vector<Replay::Command> mine;
            if (m_role == Role::Scribe && !m_programs_sent) { // писарь пишет заклинания: блоб идёт по сети, в команде — хеш
                m_programs_sent = true;
                for (const auto& [slot, text] : solution.programs) {
                    const auto program = Runes::parse_program(text);
                    if (!program) continue;
                    mine.push_back(SpellSim::set_program_command(slot, m_net.submit_blob(Runes::encode_program(*program))));
                }
            }
            if (m_role == Role::Pilot && solution.policy) solution.policy(m_sim, mine); // пилот смотрит в свой мир, ходит и кастует
            m_net.submit(mine);
        }
        if (!m_net.ready()) return;

        const auto span = m_net.advance();
        std::vector<Replay::Command> commands(span.begin(), span.end());
        for (const Replay::Command& c : commands) { // Lockstep гарантирует, что блоб пришёл: отдаём программу своей симуляции
            if (const auto hash = SpellSim::blob_reference(c); hash && !m_sim.has_program(*hash)) (void)m_sim.provide_program(*m_net.blob(*hash));
        }
        if (cheat && m_role == Role::Scribe && m_sim.tick_number() == 20) { // мимо сети: лишний каст только в мире писаря — миры уйдут друг от друга
            commands.push_back(SpellSim::cast_command(0, Runes::ManaSource::Personal, {Math::Fixed{}, Math::Fixed::from_int(-1), Math::Fixed{}}));
        }
        m_sim.tick(commands);
        m_referee.observe(m_sim);
        m_net.report_hashes(m_sim.hashes());
    }

private:
    Role m_role;
    const Challenge::Level& m_level;
    SpellSim::Simulation m_sim;
    Challenge::Referee m_referee;
    Net::Lockstep m_net;
    bool m_programs_sent = false;
};

} // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    const Challenge::Level* level = Challenge::find_level(options.level);
    const Challenge::Solution* solution = level != nullptr ? Challenge::reference_solution(options.level) : nullptr;
    if (level == nullptr || solution == nullptr) {
        std::printf("ОШИБКА: нет уровня «%s»\n", options.level.c_str());
        return 1;
    }
    std::printf("CoopScribe: уровень «%s» — %s\n  сеть: задержка %u, разброс %u, потери %u‰, дубли %u‰, порча %u‰; задержка ввода %u тиков%s\n",
                level->id.c_str(), level->title.c_str(), options.link.latency_steps, options.link.jitter_steps, options.link.loss_permille,
                options.link.duplicate_permille, options.link.corrupt_permille, options.input_delay, options.cheat ? "; ЧИТ у писаря" : "");

    Net::LoopbackNetwork network(2, options.link, options.net_seed);
    Player pilot(Player::Role::Pilot, *level, network.endpoint(0), 0, options.input_delay);
    Player scribe(Player::Role::Scribe, *level, network.endpoint(1), 1, options.input_delay);

    std::uint64_t frames = 0;
    while (frames < 400000 && !(pilot.done() && scribe.done()) && !pilot.net().desync() && !scribe.net().desync()) {
        pilot.frame(*solution, options.cheat);
        scribe.frame(*solution, options.cheat);
        network.step();
        ++frames;
    }
    // Хвост: закончивший игрок продолжает слать; даём сети дожить, чтобы последние хеши дошли и сверились.
    for (int i = 0; i < 200 && !pilot.net().desync() && !scribe.net().desync(); ++i) { pilot.frame(*solution, options.cheat); scribe.frame(*solution, options.cheat); network.step(); }

    const auto& stats = network.stats();
    std::printf("  кадров %llu; пакетов %llu, потеряно %llu, испорчено %llu; у пилота отброшено контрольной суммой %llu, простоев %llu\n",
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(stats.sent), static_cast<unsigned long long>(stats.lost),
                static_cast<unsigned long long>(stats.corrupted), static_cast<unsigned long long>(pilot.net().stats().bad_packets),
                static_cast<unsigned long long>(pilot.net().stats().stalled_pumps));

    const Net::Lockstep* detected = pilot.net().desync() ? &pilot.net() : scribe.net().desync() ? &scribe.net() : nullptr;
    if (options.cheat) {
        if (detected == nullptr) {
            std::printf("ОШИБКА: расхождение не обнаружено (тики %u/%u: %s | %s)\n", pilot.sim().tick_number(), scribe.sim().tick_number(),
                        pilot.sim().hashes().describe().c_str(), scribe.sim().hashes().describe().c_str());
            return 1;
        }
        const Net::Desync& d = *detected->desync();
        std::printf("  РАСХОЖДЕНИЕ найдено на тике %u (пир %u), подсистемы:", d.tick, static_cast<unsigned>(d.peer));
        for (const std::string& name : d.subsystems) std::printf(" %s", name.c_str());
        std::printf("\nOK (читера поймали)\n");
        return 0;
    }
    if (detected != nullptr) {
        std::printf("ОШИБКА: неожиданное расхождение на тике %u\n", detected->desync()->tick);
        return 1;
    }
    std::printf("  пилот:  %s\n  писарь: %s\n", pilot.referee().status_line().c_str(), scribe.referee().status_line().c_str());
    if (!(pilot.done() && scribe.done())) {
        std::printf("ОШИБКА: игра не закончилась (сеть не справилась)\n");
        return 1;
    }
    if (pilot.sim().tick_number() != scribe.sim().tick_number() || !(pilot.sim().hashes() == scribe.sim().hashes())) {
        std::printf("ОШИБКА: миры разошлись: %s | %s\n", pilot.sim().hashes().describe().c_str(), scribe.sim().hashes().describe().c_str());
        return 1;
    }
    std::printf("  миры совпали побитно на тике %u: %s\n", pilot.sim().tick_number(), pilot.sim().hashes().describe().c_str());
    if (pilot.referee().status() != Challenge::Status::Won) {
        std::printf("ОШИБКА: уровень не пройден\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
