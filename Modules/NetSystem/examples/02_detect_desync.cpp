/**
 * @file 02_detect_desync.cpp
 * @brief Как ловится рассинхронизация: у одного узла «баг» в симуляции (использует то, что зависит от машины).
 *
 * Узлы сверяют хеши состояния каждые 30 тиков. Несовпадение — Desync с номером тика и узла: по нему видно,
 * на каком контрольном тике и с кем разошлись, а значит и в каком промежутке искать баг.
 */

#include <NetSystem/NetSystem.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace ns = NetSystem;

struct Command {
    std::int32_t push = 0;
};

int main() {
    ns::SimulatedNetwork net(7, {.latency_us = 30'000});
    ns::ITransport& a = net.add_endpoint();
    ns::ITransport& b = net.add_endpoint();

    std::int64_t state_a = 0, state_b = 0;
    const auto make = [&](ns::ITransport& transport, ns::PeerId self, std::int64_t& state, bool buggy) {
        ns::LockstepNodeConfig config;
        config.lockstep.self = self;
        config.lockstep.players = {0, 1};
        config.lockstep.neighbors = {static_cast<ns::PeerId>(1 - self)};
        return std::make_unique<ns::LockstepNode<Command>>(
            transport, config, [](ns::PeerId p, std::uint32_t t) { return Command{static_cast<std::int32_t>(p + t % 7)}; },
            [&state, buggy](std::span<const Command> inputs, std::uint32_t tick) {
                for (const Command& c : inputs) state += c.push;
                if (buggy && tick == 100) state += 1; // «баг»: на одном узле результат вычисления чуть другой
            },
            [&state] { return static_cast<std::uint64_t>(state) * 0x9E3779B97F4A7C15ull; });
    };
    auto node_a = make(a, 0, state_a, false);
    auto node_b = make(b, 1, state_b, true);

    for (std::uint64_t now = 0; now <= 6'000'000; now += 1000) {
        net.advance_to(now);
        node_a->update(now);
        node_b->update(now);
    }

    for (const auto* node : {node_a.get(), node_b.get()}) {
        for (const ns::Desync& d : node->session().desyncs()) {
            std::printf("узел %u: рассинхронизация с узлом %u на тике %u (мой хеш %016llx, чужой %016llx)\n", node->session().config().self,
                        d.peer, d.tick, static_cast<unsigned long long>(d.local_hash), static_cast<unsigned long long>(d.remote_hash));
            break; // достаточно первой
        }
    }
    // Ожидаем: оба узла нашли первое расхождение на тике 120 (первая контрольная точка после тика 100).
    const bool ok = !node_a->session().desyncs().empty() && !node_b->session().desyncs().empty() &&
                    node_a->session().desyncs().front().tick == 120 && node_b->session().desyncs().front().tick == 120;
    return ok ? 0 : 1;
}
