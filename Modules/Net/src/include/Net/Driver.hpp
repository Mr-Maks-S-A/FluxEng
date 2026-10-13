#pragma once
/**
 * @file Driver.hpp
 * @brief Кадр сетевой игры в один вызов: `pump` → ввод → (готовы?) `advance` → блобы → запись → `tick` → хеши.
 *
 * Тот же цикл, что `Replay::Driver` делает для одиночной игры, но команды приходят из `Lockstep`. Без него каждая игра
 * переписывала одни и те же восемь строк, и в каждой по-своему (где-то забывали отдать блоб симуляции раньше команды).
 *
 * @code
 * Net::LockstepDriver driver(sim, lockstep);                 // + &session — записать объединённый ввод партии
 * // каждый кадр:
 * driver.frame([&](std::vector<Replay::Command>& mine) { mine.push_back(my_move()); });
 * @endcode
 *
 * Симуляция должна быть `Replay::Simulatable`; если у неё есть `provide_blob(bytes)`, блоб команды отдаётся ей до тика.
 */

#include <Net/Lockstep.hpp>

#include <functional>
#include <set>
#include <vector>

namespace Net {

template<Replay::Simulatable Sim>
class LockstepDriver {
public:
    /// @param record Необязательная запись: команды (после слияния) и блобы идут в неё раньше исполнения.
    LockstepDriver(Sim& sim, Lockstep& net, Replay::Session* record = nullptr) : m_sim(&sim), m_net(&net), m_record(record) {}

    /// @brief Подключает (или, с `nullptr`, отключает) запись партии: сессия должна жить, пока подключена.
    void set_record(Replay::Session* record) noexcept { m_record = record; }

    /// @brief Не исполнять тики с номером ≥ `tick` (но продолжать слать и принимать пакеты). Нужно, чтобы сравнивать пиров на одном тике.
    void set_tick_limit(std::uint32_t tick) noexcept { m_limit = tick; }
    [[nodiscard]] std::uint32_t executed_ticks() const noexcept { return m_executed; }

    /**
     * @brief Один кадр. `input(std::vector<Command>&)` вызывается, только когда сети нужен ввод на очередной тик.
     * `tamper(std::vector<Command>&)` (необязательно) видит объединённые команды прямо перед исполнением; нужен только
     * тестам и «читам», чтобы сломать один мир мимо сети.
     * @return true — тик исполнен; false — кадр без шага (сеть отстаёт или достигнут предел).
     */
    template<typename Input, typename Tamper = std::nullptr_t>
    bool frame(Input&& input, Tamper&& tamper = nullptr) {
        m_net->pump();
        if (m_net->needs_input()) {
            m_mine.clear();
            input(m_mine);
            m_net->submit(m_mine);
        }
        const std::uint32_t tick = m_sim->tick_number();
        if (tick >= m_limit || !m_net->ready()) return false;

        const std::span<const Replay::Command> merged = m_net->advance();
        m_commands.assign(merged.begin(), merged.end());
        deliver_blobs(tick);
        if constexpr (!std::is_same_v<std::decay_t<Tamper>, std::nullptr_t>) tamper(m_commands);
        const std::span<const Replay::Command> applied = m_record ? m_record->begin_tick(tick, m_commands) : std::span<const Replay::Command>(m_commands);
        m_sim->tick(applied);
        m_net->report_hashes(std::as_const(*m_sim).hashes());
        ++m_executed;
        return true;
    }

private:
    /// Lockstep не отдаёт команду, пока её блоб не пришёл: блоб — симуляции (один раз) и в запись, раньше команды.
    void deliver_blobs(std::uint32_t tick) {
        const Lockstep::BlobReference& reference = m_net->blob_reference();
        if (!reference) return;
        for (const Replay::Command& c : m_commands) {
            const auto hash = reference(c);
            if (!hash) continue;
            const std::vector<std::byte>* bytes = m_net->blob(*hash);
            if (bytes == nullptr) continue; // не должно случиться: ready() требует блоб
            if constexpr (requires(Sim& s, std::span<const std::byte> b) { s.provide_blob(b); }) {
                if (m_provided.insert(*hash).second) m_sim->provide_blob(*bytes);
            }
            if (m_record) (void)m_record->add_blob(tick, *bytes);
        }
    }

    Sim* m_sim;
    Lockstep* m_net;
    Replay::Session* m_record;
    std::uint32_t m_limit = ~0u;
    std::uint32_t m_executed = 0;
    std::set<std::uint64_t> m_provided;
    std::vector<Replay::Command> m_mine, m_commands;
};

} // namespace Net
