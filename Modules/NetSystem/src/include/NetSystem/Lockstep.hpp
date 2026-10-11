#pragma once
/**
 * @file Lockstep.hpp
 * @brief Детерминированный lockstep: каждый узел крутит свою копию симуляции, по сети ходят только команды ввода.
 *
 * ```
 *   узел A ──┐                     тик t выполняется на узле, когда у него есть команды ВСЕХ игроков на тик t.
 *   узел B ──┼── команды ввода ──► Одинаковые команды + детерминированная симуляция = одинаковое состояние
 *   узел C ──┘                     на всех узлах без пересылки самого состояния.
 * ```
 *
 * **Задержка ввода.** Команда, снятая на тике `t`, применяется на тике `t + input_delay`. Пока она летит по сети,
 * симуляция идёт на ранее объявленных командах и не ждёт. Если задержка сети больше `input_delay` тиков — узел
 * простаивает (stall), симуляция при этом не расходится.
 *
 * **Надёжность поверх ненадёжной сети.** Каждый узел шлёт соседям команды, о получении которых сосед ещё не сообщил
 * (накопительное подтверждение `need` по каждому игроку), повторяя их каждые `resend_interval_us`. Потеря, дубликат
 * и перестановка пакетов не ломают протокол. Узлы пересылают и чужие команды, поэтому подходят обе топологии:
 *  - **звезда** (сервер-ретранслятор): клиенты знают только сервер, сервер знает всех (`relay = true`); сервер сам тоже симулирует;
 *  - **mesh / p2p**: каждый знает каждого напрямую (`relay = false`: каждый шлёт только свои команды).
 *
 * **Проверка детерминизма.** Каждые `checkpoint_interval` тиков узел сообщает соседям хеш своего состояния; несовпадение
 * хешей на одном тике — рассинхронизация (Desync): её видят оба конца.
 *
 * Команда `Cmd` — структура из целых чисел без padding (`has_unique_object_representations`): значения не зависят
 * от платформы и сравнимы побайтно. Числа с плавающей точкой в команде запрещены намеренно.
 */

#include <NetSystem/Bytes.hpp>
#include <NetSystem/Transport.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace NetSystem {

struct LockstepConfig {
    PeerId self = 0;
    std::vector<PeerId> players;       ///< Все участники симуляции; PeerId совпадает с номером игрока. Должен содержать `self`.
    std::vector<PeerId> neighbors;     ///< С кем обмениваемся напрямую (звезда: клиент — сервер; сервер — все клиенты; mesh — все).
    std::uint32_t input_delay = 4;     ///< Тиков между снятием команды и её применением.
    std::uint32_t max_window = 24;     ///< Тиков одного игрока в одном пакете.
    std::uint64_t resend_interval_us = 25'000;
    std::uint32_t checkpoint_interval = 30; ///< Хеш состояния — каждые N тиков.
    bool relay = true;                 ///< Пересылать соседям и чужие команды. Нужно серверу звезды; в mesh, где все связаны напрямую,
                                       ///< выключите: трафик на узел падает в разы (каждый шлёт только свои команды).
};

struct LockstepStats {
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bad_packets = 0;     ///< Не разобрались или пришли не от соседа.
    std::uint64_t input_conflicts = 0; ///< Две разные команды одного игрока на один тик (в честной сети — всегда 0).
    std::uint64_t desyncs = 0;         ///< Несовпадений хеша состояния.
};

/// @brief Несовпадение хеша состояния на тике `tick` с узлом `peer`.
struct Desync {
    std::uint32_t tick = 0;
    PeerId peer = kNoPeer;
    std::uint64_t local_hash = 0;
    std::uint64_t remote_hash = 0;
};

template<typename Cmd>
    requires(std::has_unique_object_representations_v<Cmd> && std::is_trivially_copyable_v<Cmd> && std::is_default_constructible_v<Cmd>)
class Lockstep {
public:
    Lockstep(ITransport& transport, LockstepConfig config) : m_transport(transport), m_config(std::move(config)) {
        std::ranges::sort(m_config.players);
        m_config.players.erase(std::unique(m_config.players.begin(), m_config.players.end()), m_config.players.end());
        const auto self = std::ranges::find(m_config.players, m_config.self);
        if (self == m_config.players.end()) throw std::invalid_argument("Lockstep: players must contain self");
        m_self_index = static_cast<std::size_t>(self - m_config.players.begin());
        if (m_config.max_window == 0 || m_config.checkpoint_interval == 0) throw std::invalid_argument("Lockstep: zero window or checkpoint interval");

        // Первые input_delay тиков у всех игроков — пустые команды: так симуляция может стартовать, не дожидаясь сети.
        m_inputs.resize(m_config.players.size());
        for (PlayerInputs& in : m_inputs) in.cmds.assign(m_config.input_delay, Cmd{});
        for (const PeerId id : m_config.neighbors) {
            if (id == m_config.self) continue;
            m_neighbors.push_back(Neighbor{id, std::vector<std::uint32_t>(m_config.players.size(), m_config.input_delay), 0, false});
        }
        m_current.resize(m_config.players.size());
    }

    // ================================================================= цикл узла

    /// @brief Принять пакеты и (по таймеру или после нового ввода) отправить соседям команды и подтверждения.
    /// @param now_us Монотонное время узла в микросекундах.
    void pump(std::uint64_t now_us) {
        Packet packet;
        while (m_transport.receive(packet)) handle(packet);
        for (Neighbor& neighbor : m_neighbors) {
            if (m_force_send || !neighbor.sent_once || now_us - neighbor.last_send_us >= m_config.resend_interval_us) {
                send_to(neighbor);
                neighbor.last_send_us = now_us;
                neighbor.sent_once = true;
            }
        }
        m_force_send = false;
    }

    /// @brief Нужна ли ещё локальная команда (чтобы на тик `sim_tick + input_delay` она была объявлена).
    [[nodiscard]] bool needs_local_input() const noexcept { return m_inputs[m_self_index].end() <= m_sim_tick + m_config.input_delay; }

    /// @brief Тик, на который попадёт следующая submit_local().
    [[nodiscard]] std::uint32_t next_local_tick() const noexcept { return m_inputs[m_self_index].end(); }

    void submit_local(const Cmd& command) {
        m_inputs[m_self_index].cmds.push_back(command);
        m_force_send = true;
    }

    /// @brief Есть команды всех игроков на текущий тик.
    [[nodiscard]] bool can_advance() const noexcept {
        return std::ranges::all_of(m_inputs, [this](const PlayerInputs& in) { return in.end() > m_sim_tick; });
    }

    /// @brief Команды всех игроков на текущий тик в порядке `players` (по возрастанию PeerId). Только если can_advance().
    [[nodiscard]] std::span<const Cmd> inputs() const {
        for (std::size_t i = 0; i < m_inputs.size(); ++i) m_current[i] = m_inputs[i].cmds[m_sim_tick - m_inputs[i].base];
        return m_current;
    }

    /// @brief Тик выполнен: перейти к следующему. Только если can_advance().
    void advance() {
        ++m_sim_tick;
        prune();
    }

    /// @brief Хеш состояния после выполненных тиков (`tick` = число выполненных тиков). Считается только на контрольных тиках.
    [[nodiscard]] bool is_checkpoint(std::uint32_t tick) const noexcept { return tick != 0 && tick % m_config.checkpoint_interval == 0; }

    void report_checkpoint(std::uint32_t tick, std::uint64_t hash) {
        if (!is_checkpoint(tick)) return;
        m_local_cp[tick] = hash;
        while (m_local_cp.size() > kKeepCheckpoints) m_local_cp.erase(m_local_cp.begin());
        for (const Neighbor& neighbor : m_neighbors) {
            if (const auto it = m_remote_cp.find({neighbor.id, tick}); it != m_remote_cp.end()) {
                compare(neighbor.id, tick, hash, it->second);
                m_remote_cp.erase(it);
            }
        }
    }

    // ================================================================= состояние

    [[nodiscard]] std::uint32_t sim_tick() const noexcept { return m_sim_tick; }
    [[nodiscard]] const LockstepStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] const std::vector<Desync>& desyncs() const noexcept { return m_desyncs; }
    [[nodiscard]] const LockstepConfig& config() const noexcept { return m_config; }
    [[nodiscard]] std::size_t player_count() const noexcept { return m_config.players.size(); }
    /// @brief Тиков, на которые известны команды всех игроков (с запасом `input_delay` это не меньше sim_tick).
    [[nodiscard]] std::uint32_t ready_until() const noexcept {
        std::uint32_t result = m_inputs.front().end();
        for (const PlayerInputs& in : m_inputs) result = std::min(result, in.end());
        return result;
    }

private:
    static constexpr std::uint8_t kInputs = 1;
    static constexpr std::uint8_t kAck = 2;
    static constexpr std::size_t kKeepCheckpoints = 32;
    static constexpr std::size_t kMaxPendingRemote = 256;
    static constexpr std::size_t kAckCheckpoints = 4;

    struct PlayerInputs {
        std::deque<Cmd> cmds;
        std::uint32_t base = 0; ///< Тик первой хранимой команды.
        [[nodiscard]] std::uint32_t end() const noexcept { return base + static_cast<std::uint32_t>(cmds.size()); }
    };
    struct Neighbor {
        PeerId id = kNoPeer;
        std::vector<std::uint32_t> need; ///< По игрокам: с какого тика сосед ещё не имеет команд (его накопительное подтверждение).
        std::uint64_t last_send_us = 0;
        bool sent_once = false;
    };

    [[nodiscard]] std::optional<std::size_t> player_index(PeerId id) const noexcept {
        const auto it = std::ranges::lower_bound(m_config.players, id);
        if (it == m_config.players.end() || *it != id) return std::nullopt;
        return static_cast<std::size_t>(it - m_config.players.begin());
    }

    void handle(const Packet& packet) {
        const auto neighbor = std::ranges::find(m_neighbors, packet.from, &Neighbor::id);
        if (neighbor == m_neighbors.end()) {
            ++m_stats.bad_packets;
            return;
        }
        ++m_stats.packets_received;
        ByteReader in(packet.data);
        const auto type = in.read<std::uint8_t>();
        const bool ok = type == kInputs ? handle_inputs(in) : type == kAck ? handle_ack(in, *neighbor) : false;
        if (!ok || !in.finished()) ++m_stats.bad_packets;
    }

    bool handle_inputs(ByteReader& in) {
        const auto player = in.read<std::uint16_t>();
        const auto first = in.read<std::uint32_t>();
        const auto count = in.read<std::uint16_t>();
        const auto bytes = in.read_bytes(static_cast<std::size_t>(count) * sizeof(Cmd));
        const auto index = player_index(player);
        if (!in.ok() || !index) return false;
        PlayerInputs& inputs = m_inputs[*index];
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::uint32_t tick = first + i;
            Cmd command{};
            std::memcpy(&command, bytes.data() + static_cast<std::size_t>(i) * sizeof(Cmd), sizeof(Cmd));
            if (tick < inputs.base) continue; // давно использовано и забыто — устаревший дубликат
            if (tick < inputs.end()) {
                if (std::memcmp(&inputs.cmds[tick - inputs.base], &command, sizeof(Cmd)) != 0) ++m_stats.input_conflicts;
            } else if (tick == inputs.end()) {
                inputs.cmds.push_back(command);
            } else {
                break; // дыра: отправитель начинает с нашего подтверждения, так не бывает; ждём повтор
            }
        }
        return true;
    }

    bool handle_ack(ByteReader& in, Neighbor& neighbor) {
        const auto count = in.read<std::uint16_t>();
        if (count > m_config.players.size()) return false;
        for (std::uint16_t i = 0; i < count; ++i) {
            const auto player = in.read<std::uint16_t>();
            const auto need = in.read<std::uint32_t>();
            if (const auto index = player_index(player); index && in.ok()) {
                neighbor.need[*index] = std::max(neighbor.need[*index], need);
            }
        }
        const auto checkpoints = in.read<std::uint8_t>();
        if (checkpoints > kAckCheckpoints) return false;
        for (std::uint8_t i = 0; i < checkpoints; ++i) {
            const auto tick = in.read<std::uint32_t>();
            const auto hash = in.read<std::uint64_t>();
            if (in.ok()) on_remote_checkpoint(neighbor.id, tick, hash);
        }
        return in.ok();
    }

    void on_remote_checkpoint(PeerId peer, std::uint32_t tick, std::uint64_t hash) {
        if (const auto local = m_local_cp.find(tick); local != m_local_cp.end()) {
            compare(peer, tick, local->second, hash);
        } else if (m_local_cp.empty() || tick > m_local_cp.rbegin()->first) { // мы ещё не дошли до этого тика
            m_remote_cp[{peer, tick}] = hash;
            while (m_remote_cp.size() > kMaxPendingRemote) m_remote_cp.erase(m_remote_cp.begin());
        }
    }

    void compare(PeerId peer, std::uint32_t tick, std::uint64_t local, std::uint64_t remote) {
        if (local == remote || !m_desync_seen.insert({peer, tick}).second) return;
        ++m_stats.desyncs;
        m_desyncs.push_back(Desync{tick, peer, local, remote});
    }

    void send(Neighbor& neighbor, const ByteWriter& message) {
        m_transport.send(neighbor.id, message.bytes());
        ++m_stats.packets_sent;
        m_stats.bytes_sent += message.size();
    }

    void send_to(Neighbor& neighbor) {
        for (std::size_t j = 0; j < m_inputs.size(); ++j) {
            if (m_config.players[j] == neighbor.id) continue; // свои команды сосед знает сам
            if (!m_config.relay && j != m_self_index) continue;
            const PlayerInputs& inputs = m_inputs[j];
            const std::uint32_t from = std::max(neighbor.need[j], inputs.base);
            if (from >= inputs.end()) continue;
            const auto count = static_cast<std::uint16_t>(std::min<std::uint32_t>(inputs.end() - from, m_config.max_window));
            ByteWriter message;
            message.write<std::uint8_t>(kInputs).write<std::uint16_t>(m_config.players[j]).write<std::uint32_t>(from).write<std::uint16_t>(count);
            for (std::uint16_t i = 0; i < count; ++i) message.write(inputs.cmds[from - inputs.base + i]);
            send(neighbor, message);
        }
        ByteWriter ack;
        ack.write<std::uint8_t>(kAck).write<std::uint16_t>(static_cast<std::uint16_t>(m_inputs.size()));
        for (std::size_t j = 0; j < m_inputs.size(); ++j) ack.write<std::uint16_t>(m_config.players[j]).write<std::uint32_t>(m_inputs[j].end());
        const std::size_t sending = std::min(m_local_cp.size(), kAckCheckpoints);
        ack.write<std::uint8_t>(static_cast<std::uint8_t>(sending));
        auto it = m_local_cp.end();
        std::advance(it, -static_cast<std::ptrdiff_t>(sending));
        for (; it != m_local_cp.end(); ++it) ack.write<std::uint32_t>(it->first).write<std::uint64_t>(it->second);
        send(neighbor, ack);
    }

    // Команды нужны, пока их не использовали мы и пока сосед не подтвердил получение (повторная отправка).
    void prune() {
        for (std::size_t j = 0; j < m_inputs.size(); ++j) {
            std::uint32_t floor = m_sim_tick;
            for (const Neighbor& neighbor : m_neighbors) {
                if (m_config.players[j] != neighbor.id) floor = std::min(floor, neighbor.need[j]);
            }
            PlayerInputs& inputs = m_inputs[j];
            while (inputs.base < floor && !inputs.cmds.empty()) {
                inputs.cmds.pop_front();
                ++inputs.base;
            }
        }
    }

    ITransport& m_transport;
    LockstepConfig m_config;
    std::size_t m_self_index = 0;
    std::vector<PlayerInputs> m_inputs;
    std::vector<Neighbor> m_neighbors;
    mutable std::vector<Cmd> m_current;
    std::uint32_t m_sim_tick = 0;
    bool m_force_send = true;
    LockstepStats m_stats;

    std::map<std::uint32_t, std::uint64_t> m_local_cp;
    std::map<std::pair<PeerId, std::uint32_t>, std::uint64_t> m_remote_cp;
    std::set<std::pair<PeerId, std::uint32_t>> m_desync_seen;
    std::vector<Desync> m_desyncs;
};

// ===================================================================== узел: темп тиков поверх Lockstep

struct LockstepNodeConfig {
    LockstepConfig lockstep;
    std::uint64_t tick_interval_us = 33'333; ///< Длительность тика (30 Гц).
    double clock_scale = 1.0;                ///< Скорость часов узла (0.98 — отстаёт, 1.02 — торопится): проверка рассинхронизации темпа.
    std::uint32_t max_catchup = 8;           ///< Тиков за один update() при отставании.
};

/// @brief Узел: Lockstep + темп. Сам снимает ввод, ждёт команды остальных и вызывает шаг симуляции.
///
/// Шаг симуляции получает команды всех игроков и номер тика и обязан быть детерминированным.
template<typename Cmd>
class LockstepNode {
public:
    using SampleFn = std::function<Cmd(PeerId self, std::uint32_t tick)>;                ///< Ввод игрока на тик.
    using StepFn = std::function<void(std::span<const Cmd> inputs, std::uint32_t tick)>; ///< Один тик симуляции.
    using HashFn = std::function<std::uint64_t()>;                                       ///< Хеш состояния (на контрольных тиках).

    LockstepNode(ITransport& transport, LockstepNodeConfig config, SampleFn sample, StepFn step, HashFn hash)
        : m_config(config), m_session(transport, std::move(config.lockstep)), m_sample(std::move(sample)),
          m_step(std::move(step)), m_hash(std::move(hash)) {}

    /// @brief Один оборот цикла узла в момент `now_us` (виртуальное время старта — 0).
    void update(std::uint64_t now_us) {
        m_session.pump(now_us);
        const auto due = static_cast<std::uint64_t>(static_cast<double>(now_us) * m_config.clock_scale / static_cast<double>(m_config.tick_interval_us));
        bool submitted = false;
        std::uint32_t steps = 0;
        for (;;) {
            while (m_session.needs_local_input()) {
                m_session.submit_local(m_sample(m_session.config().self, m_session.next_local_tick()));
                submitted = true;
            }
            if (m_session.sim_tick() >= due || steps >= m_config.max_catchup) break;
            if (!m_session.can_advance()) {
                ++m_stalls;
                break;
            }
            const std::uint32_t tick = m_session.sim_tick();
            m_step(m_session.inputs(), tick);
            m_session.advance();
            if (m_session.is_checkpoint(m_session.sim_tick())) m_session.report_checkpoint(m_session.sim_tick(), m_hash());
            ++steps;
        }
        if (submitted) m_session.pump(now_us); // новый ввод — сразу в сеть, не ждать следующего оборота
    }

    [[nodiscard]] Lockstep<Cmd>& session() noexcept { return m_session; }
    [[nodiscard]] const Lockstep<Cmd>& session() const noexcept { return m_session; }
    [[nodiscard]] std::uint32_t tick() const noexcept { return m_session.sim_tick(); }
    /// @brief Сколько раз узел хотел выполнить тик, но ждал чужих команд.
    [[nodiscard]] std::uint64_t stalls() const noexcept { return m_stalls; }

private:
    LockstepNodeConfig m_config;
    Lockstep<Cmd> m_session;
    SampleFn m_sample;
    StepFn m_step;
    HashFn m_hash;
    std::uint64_t m_stalls = 0;
};

} // namespace NetSystem
