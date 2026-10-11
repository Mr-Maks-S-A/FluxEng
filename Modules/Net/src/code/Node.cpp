#include <Net/Node.hpp>

namespace Net {

Node::Node(Transport& transport, NodeConfig config, SampleFn sample, StepFn step, HashFn hashes)
    : m_config(config), m_session(transport, config.lockstep), m_sample(std::move(sample)), m_step(std::move(step)), m_hashes(std::move(hashes)) {}

void Node::update(std::uint64_t now_us) {
    m_session.pump();
    const auto due = static_cast<std::uint64_t>(static_cast<double>(now_us) * m_config.clock_scale / static_cast<double>(m_config.tick_interval_us));
    std::uint32_t steps = 0;
    for (;;) {
        while (m_session.needs_input()) {
            m_commands.clear();
            m_sample(m_config.lockstep.local, m_session.next_input_tick(), m_commands);
            m_session.submit(m_commands);
        }
        if (m_session.tick() >= due || steps >= m_config.max_catchup) break;
        if (!m_session.ready()) {
            if (m_session.connected()) ++m_stalls;
            break;
        }
        const std::uint32_t tick = m_session.tick();
        m_step(m_session.advance(), tick);
        if (m_config.hash_interval != 0 && m_session.tick() % m_config.hash_interval == 0) m_session.report_hashes(m_hashes());
        ++steps;
    }
    m_session.pump(); // новый ввод и подтверждения — сразу в сеть, не ждать следующего оборота
}

} // namespace Net
