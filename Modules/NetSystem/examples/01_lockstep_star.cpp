/**
 * @file 01_lockstep_star.cpp
 * @brief Три узла смотрят на одну симуляцию: сервер-ретранслятор и два клиента, плохая сеть (потери, джиттер).
 *
 * Между узлами ходят только команды ввода. «Симуляция» — счётчик, который каждый узел считает у себя;
 * в конце хеши состояния совпадают. Тот же код работает с настоящим транспортом: нужно лишь заменить
 * SimulatedNetwork на реализацию ITransport поверх сокетов.
 */

#include <NetSystem/NetSystem.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace ns = NetSystem;

struct Command {
    std::int16_t push = 0; // «толкнуть счётчик» на столько
    std::int16_t pad = 0;  // явное выравнивание: у команды не должно быть padding
};

struct Counter {
    std::int64_t value = 0;
    std::uint64_t hash = 14695981039346656037ull;

    void step(std::span<const Command> inputs, std::uint32_t tick) {
        for (const Command& c : inputs) {
            value += c.push;
            hash = (hash ^ static_cast<std::uint64_t>(value)) * 1099511628211ull;
        }
        hash = (hash ^ tick) * 1099511628211ull;
    }
};

int main() {
    constexpr int kPlayers = 3;
    ns::SimulatedNetwork net(/*seed*/ 2024, {.latency_us = 45'000, .jitter_us = 30'000, .loss = 0.15, .duplicate = 0.05});

    std::vector<ns::ITransport*> transports;
    for (int i = 0; i < kPlayers; ++i) transports.push_back(&net.add_endpoint());

    std::vector<Counter> counters(kPlayers);
    std::vector<std::unique_ptr<ns::LockstepNode<Command>>> nodes;
    for (int i = 0; i < kPlayers; ++i) {
        ns::LockstepNodeConfig config;
        config.lockstep.self = static_cast<ns::PeerId>(i);
        config.lockstep.players = {0, 1, 2};
        // Звезда: узел 0 — сервер и тоже игрок; клиенты знают только его.
        config.lockstep.neighbors = i == 0 ? std::vector<ns::PeerId>{1, 2} : std::vector<ns::PeerId>{0};
        config.lockstep.relay = i == 0; // ретранслирует только сервер
        Counter* counter = &counters[static_cast<std::size_t>(i)];
        nodes.push_back(std::make_unique<ns::LockstepNode<Command>>(
            *transports[static_cast<std::size_t>(i)], config,
            // Ввод игрока: чистая функция от (игрок, тик) — у реальной игры это клавиши и мышь, снятые на этом тике.
            [](ns::PeerId self, std::uint32_t tick) { return Command{static_cast<std::int16_t>((self + 1) * ((tick % 5) - 2)), 0}; },
            [counter](std::span<const Command> inputs, std::uint32_t tick) { counter->step(inputs, tick); },
            [counter] { return counter->hash; }));
    }

    // Виртуальное время: 1 мс за оборот, 10 секунд игры.
    for (std::uint64_t now = 0; now <= 10'000'000; now += 1000) {
        net.advance_to(now);
        for (auto& node : nodes) node->update(now);
    }

    bool same = true;
    for (int i = 0; i < kPlayers; ++i) {
        const auto& session = nodes[static_cast<std::size_t>(i)]->session();
        std::printf("узел %d: тик %u, счётчик %lld, хеш %016llx, простоев %llu, пакетов отправлено %llu\n", i, session.sim_tick(),
                    static_cast<long long>(counters[static_cast<std::size_t>(i)].value),
                    static_cast<unsigned long long>(counters[static_cast<std::size_t>(i)].hash),
                    static_cast<unsigned long long>(nodes[static_cast<std::size_t>(i)]->stalls()),
                    static_cast<unsigned long long>(session.stats().packets_sent));
    }
    // Узлы стоят на немного разных тиках: сравниваем хеши на общем контрольном тике через повторный прогон не нужно —
    // рассинхронизацию протокол ловит сам (Desync), счётчик таких событий и есть проверка.
    for (const auto& node : nodes) same = same && node->session().stats().desyncs == 0 && node->session().stats().input_conflicts == 0;
    std::printf("сеть: отправлено %llu, потеряно %llu, дубликатов %llu; рассинхронизаций нет: %s\n",
                static_cast<unsigned long long>(net.stats().sent), static_cast<unsigned long long>(net.stats().dropped),
                static_cast<unsigned long long>(net.stats().duplicated), same ? "да" : "НЕТ");
    return same && nodes[0]->tick() > 200 ? 0 : 1;
}
