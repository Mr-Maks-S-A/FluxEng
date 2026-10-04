/**
 * @example 01_two_peers.cpp
 * Два игрока по сети: каждый шлёт свои команды, оба исполняют одни и те же тики и получают одно и то же состояние.
 *
 * «Симуляция» — крошечный счётчик; сеть — `LoopbackNetwork` в памяти (с задержкой). Настоящий UDP подключается через
 * `Net::Transport` — протокол (`Lockstep`) про сокеты ничего не знает.
 *
 * Весь цикл игры на один кадр — четыре строки на пира: `pump`, `submit` (если нужен ввод), `advance` (если готовы), `report_hashes`.
 */

#include <Math/Hash.hpp>
#include <Net/Lockstep.hpp>

#include <cstdio>

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

enum : std::uint16_t { Add = 1 };

struct Counter {
    std::int64_t sum = 0;
    std::uint32_t ticks = 0;
    void tick(std::span<const Replay::Command> commands) {
        for (const Replay::Command& c : commands) sum += c.x;
        ++ticks;
    }
    Replay::StateHashes hashes() const {
        Math::Hasher s;
        s.add_signed(sum);
        Replay::StateHashes h;
        h.add("sum", s.value());
        return h;
    }
};

int main() {
    Net::LoopbackNetwork network(2, {.latency_steps = 2});
    Net::LockstepConfig config{.peers = 2, .seed = 42, .config_hash = 1, .input_delay = 3};
    config.local = 0;
    Net::Lockstep alice(network.endpoint(0), config);
    config.local = 1;
    Net::Lockstep bob(network.endpoint(1), config);

    Counter sim_a, sim_b;
    const auto frame = [](Net::Lockstep& net, Counter& sim, std::span<const Replay::Command> wish) {
        net.pump();                                   // принять и отправить пакеты
        if (net.needs_input()) net.submit(wish);      // свои команды — на тик tick() + input_delay
        if (net.ready()) {                            // команды всех пиров на тик есть
            sim.tick(net.advance());                  // исполняем одинаково с остальными
            net.report_hashes(sim.hashes());          // сверка хешей с остальными
        }
    };

    for (int i = 0; i < 120; ++i) {
        const Replay::Command a_cmd{.type = Add, .x = 1};
        const Replay::Command b_cmd{.type = Add, .x = 100};
        frame(alice, sim_a, i % 10 == 0 ? std::span(&a_cmd, 1) : std::span<const Replay::Command>{});
        frame(bob, sim_b, i % 15 == 0 ? std::span(&b_cmd, 1) : std::span<const Replay::Command>{});
        network.step();
    }
    std::printf("Алиса: тик %u, сумма %lld;  Боб: тик %u, сумма %lld\n", sim_a.ticks, static_cast<long long>(sim_a.sum), sim_b.ticks, static_cast<long long>(sim_b.sum));

    // Пиры могут отличаться на тик-два (кто успел шагнуть в этом кадре): догоняем, затем сравниваем.
    while (sim_a.ticks != sim_b.ticks) frame(sim_a.ticks < sim_b.ticks ? alice : bob, sim_a.ticks < sim_b.ticks ? sim_a : sim_b, {}), network.step();
    EXPECT(sim_a.ticks > 100);
    EXPECT(sim_a.sum == sim_b.sum);
    EXPECT(!alice.desync() && !bob.desync());
    std::printf("совпало: оба на тике %u, сумма %lld\n", sim_a.ticks, static_cast<long long>(sim_a.sum));
    return 0;
}
