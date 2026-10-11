#pragma once
/**
 * @file Node.hpp
 * @brief Узел lockstep: `Lockstep` + темп тиков по часам узла. Сам снимает ввод, ждёт команды остальных и вызывает шаг.
 *
 * `Lockstep` — протокол и не знает про время: его дёргают из игрового цикла. `Node` — готовый цикл для тех, у кого своего
 * нет: консольных песочниц, серверов, тестов. Он считает, сколько тиков «должно» было пройти по часам узла
 * (`now_us × clock_scale / tick_interval_us`), снимает свой ввод заранее на `input_delay` тиков и исполняет тики, пока
 * есть команды всех игроков. Часы узлов могут идти с разной скоростью (`clock_scale`): быстрый ждёт медленного, отрыв
 * не больше `input_delay + 1` тиков, результат тот же.
 *
 * ```
 *   Node node(transport, config, sample, step, hashes);
 *   каждый оборот цикла:  network.advance_to(now);  node.update(now);
 * ```
 *
 * **Команды и игроки.** Склеенные команды тика приходят в шаг в порядке номеров игроков; номера игрока в `Command` нет.
 * Если каждый игрок подаёт ровно одну команду на тик (типичный ввод «снимок управления»), номер команды = номер игрока.
 */

#include <Net/Lockstep.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace Net {

struct NodeConfig {
    LockstepConfig lockstep{};
    std::uint64_t tick_interval_us = 33'333; ///< Длительность тика (30 Гц).
    double clock_scale = 1.0;                ///< Скорость часов узла (0.98 — отстаёт, 1.02 — торопится).
    std::uint32_t max_catchup = 8;           ///< Тиков за один `update()` при отставании.
    std::uint32_t hash_interval = 30;        ///< Хеши подсистем рассылаются каждые N тиков (0 — не рассылать).
};

class Node {
public:
    /// Ввод игрока `self` на `tick`: дописать его команды в `out` (пусто — «ничего не делает»). Чистая функция от аргументов.
    using SampleFn = std::function<void(PeerId self, std::uint32_t tick, std::vector<Replay::Command>& out)>;
    /// Один тик симуляции: команды всех игроков (порядок — по номеру игрока) и номер тика. Детерминированный.
    using StepFn = std::function<void(std::span<const Replay::Command> inputs, std::uint32_t tick)>;
    /// Хеши подсистем после последнего исполненного тика.
    using HashFn = std::function<Replay::StateHashes()>;

    Node(Transport& transport, NodeConfig config, SampleFn sample, StepFn step, HashFn hashes);

    /// @brief Один оборот цикла узла в момент `now_us` (виртуальное время старта — 0).
    void update(std::uint64_t now_us);

    [[nodiscard]] Lockstep& session() noexcept { return m_session; }
    [[nodiscard]] const Lockstep& session() const noexcept { return m_session; }
    /// @brief Сколько тиков исполнено.
    [[nodiscard]] std::uint32_t tick() const noexcept { return m_session.tick(); }
    /// @brief Сколько раз узел хотел выполнить тик, но ждал чужих команд (знакомство в счёт не идёт).
    [[nodiscard]] std::uint64_t stalls() const noexcept { return m_stalls; }

private:
    NodeConfig m_config;
    Lockstep m_session;
    SampleFn m_sample;
    StepFn m_step;
    HashFn m_hashes;
    std::vector<Replay::Command> m_commands;
    std::uint64_t m_stalls = 0;
};

} // namespace Net
