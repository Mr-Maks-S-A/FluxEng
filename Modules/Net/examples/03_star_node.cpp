/**
 * @example 03_star_node.cpp
 * Звезда: сервер-ретранслятор и два клиента смотрят на одну симуляцию через плохую сеть (потери, джиттер, дубли).
 *
 * Между узлами ходят только команды ввода. «Симуляция» — счётчик, который каждый узел считает у себя; в конце хеши
 * совпадают. `Net::Node` сам держит темп тиков по часам узла, снимает ввод и ждёт чужие команды. Клиенты знают только
 * сервер (`neighbors = {0}`), а сервер пересылает чужие команды (`relay = true`). Настоящий UDP подключается
 * реализацией `Net::Transport` — протокол про сокеты ничего не знает.
 */

#include <Net/Net.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

struct Counter {
    std::int64_t value = 0;
    std::uint64_t hash = 14695981039346656037ull;
    void step(std::span<const Replay::Command> inputs, std::uint32_t tick) {
        for (const Replay::Command& c : inputs) {
            value += c.x;
            hash = (hash ^ static_cast<std::uint64_t>(value)) * 1099511628211ull;
        }
        hash = (hash ^ tick) * 1099511628211ull;
    }
};

int main() {
    constexpr int players = 3;
    Net::SimulatedNetwork net(/*seed*/ 2024, {.latency_us = 45'000, .jitter_us = 30'000, .loss = 0.15, .duplicate = 0.05});
    for (int i = 0; i < players; ++i) net.add_endpoint();

    std::vector<Counter> counters(players);
    std::vector<std::unique_ptr<Net::Node>> nodes;
    for (int i = 0; i < players; ++i) {
        Net::NodeConfig config;
        config.lockstep.local = static_cast<Net::PeerId>(i);
        config.lockstep.peers = players;
        if (i != 0) config.lockstep.neighbors = {0}; // клиенты знают только сервер; у сервера пусто — знает всех
        config.lockstep.relay = i == 0;              // ретранслирует только сервер
        Counter* counter = &counters[static_cast<std::size_t>(i)];
        nodes.push_back(std::make_unique<Net::Node>(
            net.endpoint(static_cast<Net::PeerId>(i)), config,
            // Ввод игрока: чистая функция от (игрок, тик) — у реальной игры это клавиши и мышь, снятые на этом тике.
            [](Net::PeerId self, std::uint32_t tick, std::vector<Replay::Command>& out) {
                out.push_back({.type = 1, .x = static_cast<std::int32_t>((self + 1) * (static_cast<int>(tick % 5) - 2))});
            },
            [counter](std::span<const Replay::Command> inputs, std::uint32_t tick) { counter->step(inputs, tick); },
            [counter] {
                Replay::StateHashes h;
                h.add("counter", counter->hash);
                return h;
            }));
    }

    // Виртуальное время: 1 мс за оборот, 10 секунд игры.
    for (std::uint64_t now = 0; now <= 10'000'000; now += 1000) {
        net.advance_to(now);
        for (auto& node : nodes) node->update(now);
    }

    bool same = true;
    for (int i = 0; i < players; ++i) {
        const auto& node = *nodes[static_cast<std::size_t>(i)];
        std::printf("узел %d: тик %u, счётчик %lld, простоев %llu, пакетов отправлено %llu\n", i, node.tick(),
                    static_cast<long long>(counters[static_cast<std::size_t>(i)].value), static_cast<unsigned long long>(node.stalls()),
                    static_cast<unsigned long long>(node.session().stats().packets_sent));
        same = same && !node.session().desync() && node.session().stats().input_conflicts == 0;
    }
    std::printf("сеть: отправлено %llu, потеряно %llu, дубликатов %llu; рассинхронизаций нет: %s\n", static_cast<unsigned long long>(net.stats().sent),
                static_cast<unsigned long long>(net.stats().dropped), static_cast<unsigned long long>(net.stats().duplicated), same ? "да" : "НЕТ");
    return same && nodes[0]->tick() > 200 ? 0 : 1;
}
