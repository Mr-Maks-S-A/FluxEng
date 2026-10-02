#pragma once
/**
 * @file Probe.hpp
 * @brief Модуль-зонд для тестов: пишет вызовы хуков в общий журнал и может бросать исключения.
 */

#include <RuntimeSystem/RuntimeSystem.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace probe {

using Log = std::vector<std::string>;

struct Behavior {
    bool throw_in_declare = false;
    bool throw_in_init = false;
    bool throw_in_tick = false;
};

class Probe final : public RuntimeSystem::Module {
public:
    Probe(std::string name, Log& log, std::vector<std::string> dependencies = {}, Behavior behavior = {})
        : m_name(std::move(name)), m_log(log), m_behavior(behavior) {
        for (const std::string& dependency : dependencies) depends_on(dependency);
    }

    [[nodiscard]] std::string_view name() const noexcept override { return m_name; }

    void declare(RuntimeSystem::Runtime&) override {
        m_log.push_back(m_name + ".declare");
        if (m_behavior.throw_in_declare) throw std::runtime_error(m_name + " declare failed");
    }
    void init(RuntimeSystem::Runtime&) override {
        m_log.push_back(m_name + ".init");
        if (m_behavior.throw_in_init) throw std::runtime_error(m_name + " init failed");
    }
    void frame(RuntimeSystem::Runtime&, float) override { m_log.push_back(m_name + ".frame"); }
    void tick(RuntimeSystem::Runtime&) override {
        m_log.push_back(m_name + ".tick");
        if (m_behavior.throw_in_tick) throw std::runtime_error(m_name + " tick failed");
    }
    void shutdown(RuntimeSystem::Runtime&) noexcept override { m_log.push_back(m_name + ".shutdown"); }

private:
    std::string m_name;
    Log& m_log;
    Behavior m_behavior;
};

/// Номера вхождений строки в журнале; -1, если нет.
inline int index_of(const Log& log, std::string_view entry) {
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i] == entry) return static_cast<int>(i);
    }
    return -1;
}

inline int count_of(const Log& log, std::string_view entry) {
    int count = 0;
    for (const std::string& line : log) count += line == entry ? 1 : 0;
    return count;
}

} // namespace probe
