/**
 * @example 02_bad_network_and_desync.cpp
 * Плохая сеть и расхождение. Часть 1: потери 20 %, дубли, порча, разброс — состояние всё равно одинаково, просто кадров
 * нужно больше. Часть 2: один пир «считает по-своему» (имитация недетерминизма) — сверка именованных хешей
 * называет тик и подсистему, которые разошлись.
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

struct World {
    std::int64_t gold = 0, wood = 0;
    std::uint32_t ticks = 0;
    std::int64_t drift = 0; ///< Ошибка «недетерминизма» у одного из пиров.
    void tick(std::span<const Replay::Command> commands) {
        for (const Replay::Command& c : commands) (c.type == 1 ? gold : wood) += c.x;
        gold += drift;
        ++ticks;
    }
    Replay::StateHashes hashes() const { // у каждой подсистемы — свой хеш: при расхождении видно, какая
        Math::Hasher g, w;
        g.add_signed(gold);
        w.add_signed(wood);
        Replay::StateHashes h;
        h.add("gold", g.value()).add("wood", w.value());
        return h;
    }
};

struct Pair {
    Net::LoopbackNetwork network;
    Net::Lockstep a, b;
    World wa, wb;
    explicit Pair(const Net::LinkConfig& link)
        : network(2, link, 5), a(network.endpoint(0), {.local = 0, .peers = 2, .seed = 1, .config_hash = 2}),
          b(network.endpoint(1), {.local = 1, .peers = 2, .seed = 1, .config_hash = 2}) {}
    void frame(int i) {
        const Replay::Command ca{.type = 1, .x = 5}, cb{.type = 2, .x = 3};
        for (auto [net, world, cmd] : {std::tuple<Net::Lockstep*, World*, const Replay::Command*>{&a, &wa, &ca}, {&b, &wb, &cb}}) {
            net->pump();
            if (net->needs_input()) net->submit(i % 7 == 0 ? std::span(cmd, 1) : std::span<const Replay::Command>{});
            if (net->ready()) {
                world->tick(net->advance());
                net->report_hashes(world->hashes());
            }
        }
        network.step();
    }
};

int main() {
    {
        Pair pair({.latency_steps = 3, .jitter_steps = 4, .loss_permille = 200, .duplicate_permille = 100, .corrupt_permille = 50});
        for (int i = 0; i < 1500; ++i) pair.frame(i);
        const auto& net = pair.network.stats();
        std::printf("плохая сеть: отправлено %llu, потеряно %llu, испорчено %llu; шагов %u/%u, пакетов отброшено контрольной суммой %llu\n",
                    static_cast<unsigned long long>(net.sent), static_cast<unsigned long long>(net.lost), static_cast<unsigned long long>(net.corrupted),
                    pair.wa.ticks, pair.wb.ticks, static_cast<unsigned long long>(pair.a.stats().bad_packets + pair.b.stats().bad_packets));
        EXPECT(pair.wa.ticks > 200 && pair.wb.ticks > 200);
        EXPECT(!pair.a.desync() && !pair.b.desync());
    }
    {
        Pair pair({.latency_steps = 1});
        for (int i = 0; i < 50; ++i) pair.frame(i);
        pair.wb.drift = 1; // у Боба начинается «недетерминизм»
        for (int i = 50; i < 200; ++i) pair.frame(i);
        EXPECT(pair.a.desync().has_value());
        const Net::Desync& d = *pair.a.desync();
        std::printf("расхождение: тик %u, пир %u, подсистемы:", d.tick, static_cast<unsigned>(d.peer));
        for (const std::string& s : d.subsystems) std::printf(" %s", s.c_str());
        std::printf("\n");
        EXPECT(d.subsystems.size() == 1 && d.subsystems[0] == "gold"); // wood цела: расхождение локализовано
    }
    std::printf("OK\n");
    return 0;
}
