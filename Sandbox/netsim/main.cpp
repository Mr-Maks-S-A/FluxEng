/**
 * @file main.cpp
 * @brief NetSim — несколько узлов «смотрят» на одну симуляцию через плохую сеть; проверка детерминизма и lockstep.
 *
 * Без окна и GPU. Каждый узел — свой RuntimeSystem::Runtime с модулем MagicWorld (ECS + JobSystem, только целые числа)
 * и свой транспорт; между узлами ходят лишь команды ввода (NetSystem::Lockstep). Сеть — SimulatedNetwork на виртуальном времени:
 * прогон не зависит от скорости машины и воспроизводим по `--seed`.
 *
 * Что проверяется:
 *  - состояние всех узлов совпадает на КАЖДОМ тике (не только в конце);
 *  - результат не зависит от числа потоков JobSystem на узле (`--threads 0,1,3,7` — по кругу по узлам);
 *  - выдерживает потери, джиттер, дубликаты и разную скорость часов;
 *  - внедрённая ошибка (`--corrupt-peer P --corrupt-tick T`) обнаруживается протоколом (`--expect-desync` инвертирует итог).
 *
 * Топологии: `--mode star` (узел 0 — сервер-ретранслятор и тоже игрок), `--mode mesh` (p2p, все со всеми).
 * Код выхода: 0 — всё сошлось (или, с --expect-desync, рассинхронизация поймана); 1 — иначе.
 */

#include "Sim.hpp"

#include <NetSystem/NetSystem.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ns = NetSystem;
namespace rs = RuntimeSystem;
using netsim::Cmd;
using netsim::MagicWorld;

namespace {

struct Options {
    bool mesh = false;
    int players = 4;
    std::uint32_t ticks = 600;
    double latency_ms = 40.0;
    double jitter_ms = 20.0;
    double loss = 0.05;
    double duplicate = 0.02;
    std::uint64_t seed = 1;
    std::uint32_t input_delay = 4;
    std::vector<int> threads{0, 1, 3, 7};
    bool drift = false;
    int corrupt_peer = -1;
    std::uint32_t corrupt_tick = 0;
    bool expect_desync = false;
    bool ascii = false;
};

std::vector<int> parse_list(std::string_view text) {
    std::vector<int> values;
    std::stringstream stream{std::string(text)};
    std::string item;
    while (std::getline(stream, item, ',')) values.push_back(std::atoi(item.c_str()));
    return values;
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (arg == "--mode") o.mesh = std::string_view(value()) == "mesh";
        else if (arg == "--players") o.players = std::max(1, std::atoi(value()));
        else if (arg == "--ticks") o.ticks = static_cast<std::uint32_t>(std::atoi(value()));
        else if (arg == "--latency-ms") o.latency_ms = std::atof(value());
        else if (arg == "--jitter-ms") o.jitter_ms = std::atof(value());
        else if (arg == "--loss") o.loss = std::atof(value());
        else if (arg == "--dup") o.duplicate = std::atof(value());
        else if (arg == "--seed") o.seed = static_cast<std::uint64_t>(std::atoll(value()));
        else if (arg == "--input-delay") o.input_delay = static_cast<std::uint32_t>(std::atoi(value()));
        else if (arg == "--threads") o.threads = parse_list(value());
        else if (arg == "--drift") o.drift = true;
        else if (arg == "--corrupt-peer") o.corrupt_peer = std::atoi(value());
        else if (arg == "--corrupt-tick") o.corrupt_tick = static_cast<std::uint32_t>(std::atoi(value()));
        else if (arg == "--expect-desync") o.expect_desync = true;
        else if (arg == "--ascii") o.ascii = true;
        else {
            std::fprintf(stderr, "unknown option '%s'\n", argv[i]);
            std::fprintf(stderr, "usage: NetSim [--mode star|mesh] [--players N] [--ticks N] [--latency-ms X] [--jitter-ms X] [--loss P] [--dup P]\n"
                                 "              [--seed N] [--input-delay N] [--threads 0,1,3,7] [--drift] [--corrupt-peer P --corrupt-tick T]\n"
                                 "              [--expect-desync] [--ascii]\n");
            return false;
        }
    }
    if (o.threads.empty()) o.threads = {0};
    return true;
}

/// Один узел: Runtime + модуль мира + узел lockstep + журнал хешей по тикам.
struct Peer {
    std::unique_ptr<rs::Runtime> runtime;
    MagicWorld* world = nullptr;
    std::unique_ptr<ns::LockstepNode<Cmd>> node;
    std::vector<std::uint64_t> history; // хеш после каждого тика
    int threads = 0;
};

} // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return 2;

    ns::SimulatedNetwork net(o.seed, {.latency_us = static_cast<std::uint64_t>(o.latency_ms * 1000.0),
                                      .jitter_us = static_cast<std::uint64_t>(o.jitter_ms * 1000.0), .loss = o.loss, .duplicate = o.duplicate});
    std::vector<ns::ITransport*> transports;
    for (int i = 0; i < o.players; ++i) transports.push_back(&net.add_endpoint());

    std::vector<Peer> peers(static_cast<std::size_t>(o.players));
    for (int i = 0; i < o.players; ++i) {
        Peer& peer = peers[static_cast<std::size_t>(i)];
        peer.threads = o.threads[static_cast<std::size_t>(i) % o.threads.size()];
        peer.runtime = std::make_unique<rs::Runtime>(rs::RuntimeConfig{.ticks_per_second = 30.0, .lockstep = true, .threads = peer.threads,
                                                                       .tick_arena_bytes = MemorySystem::MiB(8), .frame_arena_bytes = MemorySystem::MiB(2)});
        peer.world = &peer.runtime->add<MagicWorld>(o.seed, o.players, netsim::Corruption{o.corrupt_tick, i == o.corrupt_peer});
        peer.runtime->initialize();

        ns::LockstepNodeConfig config;
        config.lockstep.self = static_cast<ns::PeerId>(i);
        config.lockstep.input_delay = o.input_delay;
        for (int p = 0; p < o.players; ++p) config.lockstep.players.push_back(static_cast<ns::PeerId>(p));
        for (int p = 0; p < o.players; ++p) {
            const bool linked = o.mesh ? p != i : (i == 0 ? p != 0 : p == 0); // звезда: узел 0 — сервер
            if (linked) config.lockstep.neighbors.push_back(static_cast<ns::PeerId>(p));
        }
        config.lockstep.relay = !o.mesh && i == 0; // ретранслирует только сервер звезды
        if (o.drift) config.clock_scale = 0.9 + 0.07 * static_cast<double>(i % 4); // 0.90 … 1.11

        const std::uint64_t seed = o.seed;
        Peer* self = &peer;
        peer.node = std::make_unique<ns::LockstepNode<Cmd>>(
            *transports[static_cast<std::size_t>(i)], config,
            [seed](ns::PeerId player, std::uint32_t tick) { return netsim::bot_input(seed, player, tick); },
            [self](std::span<const Cmd> inputs, std::uint32_t) {
                self->world->set_inputs(inputs);
                self->runtime->tick(); // модули Runtime: MagicWorld::tick
                self->history.push_back(self->world->hash());
            },
            [self] { return self->world->hash(); });
    }

    std::printf("NetSim: %s, %d игроков, %u тиков, сеть %.0f±%.0f мс, потери %.0f%%, дубликаты %.0f%%, input_delay %u, потоки %s%s\n",
                o.mesh ? "mesh (p2p)" : "звезда (сервер-ретранслятор)", o.players, o.ticks, o.latency_ms, o.jitter_ms, o.loss * 100.0,
                o.duplicate * 100.0, o.input_delay, [&] {
                    std::string s;
                    for (const int t : o.threads) s += (s.empty() ? "" : ",") + std::to_string(t);
                    return s;
                }().c_str(), o.drift ? ", часы с дрейфом" : "");

    // Виртуальное время: 1 мс за оборот. Пока все узлы не дойдут до нужного тика (или не истёк предел 10 минут).
    std::uint64_t now = 0;
    const std::uint64_t limit = 600'000'000;
    for (; now <= limit; now += 1000) {
        net.advance_to(now);
        for (Peer& peer : peers) peer.node->update(now);
        if (std::ranges::all_of(peers, [&](const Peer& p) { return p.node->tick() >= o.ticks; })) break;
    }
    const bool finished = std::ranges::all_of(peers, [&](const Peer& p) { return p.node->tick() >= o.ticks; });

    // Сверка по каждому тику: истории хешей всех узлов совпадают на общем отрезке.
    std::size_t common = peers[0].history.size();
    for (const Peer& p : peers) common = std::min(common, p.history.size());
    std::size_t first_mismatch = common;
    for (std::size_t t = 0; t < common && first_mismatch == common; ++t) {
        for (const Peer& p : peers) {
            if (p.history[t] != peers[0].history[t]) {
                first_mismatch = t;
                break;
            }
        }
    }

    std::printf("\n%-5s %-8s %-10s %-18s %-8s %-9s %-10s %-9s %-8s\n", "узел", "потоки", "тик", "хеш (на общем тике)", "простои", "пакетов", "байт", "рассинхр.", "очки");
    std::uint64_t total_desyncs = 0;
    for (std::size_t i = 0; i < peers.size(); ++i) {
        const Peer& p = peers[i];
        const auto& stats = p.node->session().stats();
        total_desyncs += stats.desyncs;
        std::printf("%-5zu %-8d %-10u %016llx   %-8llu %-9llu %-10llu %-9llu %d\n", i, p.threads, p.node->tick(),
                    static_cast<unsigned long long>(common > 0 ? p.history[common - 1] : 0), static_cast<unsigned long long>(p.node->stalls()),
                    static_cast<unsigned long long>(stats.packets_sent), static_cast<unsigned long long>(stats.bytes_sent),
                    static_cast<unsigned long long>(stats.desyncs), p.world->score(static_cast<int>(i)));
    }
    const ns::NetworkStats& wire = net.stats();
    std::printf("\nсеть: отправлено %llu, потеряно %llu, дубликатов %llu, всего %.1f КБ; виртуального времени %.1f с\n",
                static_cast<unsigned long long>(wire.sent), static_cast<unsigned long long>(wire.dropped),
                static_cast<unsigned long long>(wire.duplicated), static_cast<double>(wire.bytes_sent) / 1024.0, static_cast<double>(now) / 1e6);
    std::printf("сверено тиков: %zu; первое расхождение хешей: %s; в симуляции: огоньков %zu, болтов %zu\n", common,
                first_mismatch == common ? "нет" : std::to_string(first_mismatch).c_str(), peers[0].world->wisps(), peers[0].world->bolts());
    if (o.ascii) std::printf("\nузел 0, тик %u:\n%s", peers[0].world->tick_count(), peers[0].world->ascii(64, 24).c_str());

    if (!finished) {
        std::printf("ИТОГ: узлы не дошли до %u тиков за отведённое время\n", o.ticks);
        return 1;
    }
    if (o.expect_desync) {
        const bool caught = total_desyncs > 0 && first_mismatch != common;
        std::printf("ИТОГ: внедрённая ошибка %s\n", caught ? "обнаружена протоколом (как и должно быть)" : "НЕ обнаружена");
        return caught ? 0 : 1;
    }
    const bool ok = first_mismatch == common && total_desyncs == 0;
    std::printf("ИТОГ: %s\n", ok ? "состояние всех узлов совпадает на каждом тике" : "РАССИНХРОНИЗАЦИЯ");
    return ok ? 0 : 1;
}
