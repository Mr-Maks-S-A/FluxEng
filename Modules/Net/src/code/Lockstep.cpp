#include <Net/Lockstep.hpp>

#include <Math/Assert.hpp>
#include <Math/Hash.hpp>

#include <algorithm>

namespace Net {

namespace {
constexpr std::uint32_t input_window = 512;       // принимаем тики не дальше этого от исполняемого
constexpr std::uint32_t hello_period = 32;        // знакомство повторяется и после соединения (дёшево, лечит потерю)
constexpr std::size_t inputs_budget = max_packet_size - 64;
constexpr std::uint32_t hash_memory = 256;        // сколько тиков хешей помним
constexpr std::size_t max_partial_blobs = 16;
} // namespace

Lockstep::Lockstep(Transport& transport, const LockstepConfig& config)
    : m_transport(transport), m_config(config),
      m_inputs(config.peers), m_have(config.peers, config.input_delay),
      m_peer_have(config.peers, std::vector<std::uint32_t>(config.peers, config.input_delay)),
      m_is_neighbor(config.peers, false), m_hello_seen(config.peers, true), m_sent_have(config.peers), m_last_heard(config.peers, 0) {
    FLUX_ASSERT(config.peers >= 1 && config.peers <= max_peers && config.local < config.peers, "Lockstep: локальный пир вне 0…peers-1");
    FLUX_ASSERT(config.input_delay >= 1, "Lockstep: задержка ввода должна быть ≥ 1 тика");
    FLUX_ASSERT(config.max_window >= 1, "Lockstep: окно тиков должно быть ≥ 1");
    if (config.neighbors.empty()) {
        for (PeerId p = 0; p < config.peers; ++p) {
            if (p != config.local) m_neighbors.push_back(p);
        }
    } else {
        for (const PeerId p : config.neighbors) {
            FLUX_ASSERT(p < config.peers && p != config.local, "Lockstep: сосед вне 0…peers-1 или он сам");
            if (!m_is_neighbor[p]) m_neighbors.push_back(p);
            m_is_neighbor[p] = true;
        }
    }
    for (const PeerId p : m_neighbors) {
        m_is_neighbor[p] = true;
        m_hello_seen[p] = false; // знакомство нужно только с соседями
    }
    m_local_next = config.input_delay;
    // Первые input_delay тиков пусты у всех (никто ещё ничего не мог подать) — иначе игра не стартует.
    for (auto& per_peer : m_inputs) {
        for (std::uint32_t t = 0; t < config.input_delay; ++t) per_peer[t] = {};
    }
}

bool Lockstep::connected() const noexcept {
    return m_failure.empty() && std::ranges::all_of(m_hello_seen, [](bool b) { return b; });
}

void Lockstep::send_packet(PeerId to, const Body& body) {
    const std::vector<std::byte> bytes = encode(Packet{m_config.local, body});
    m_transport.send(to, bytes);
    ++m_stats.packets_sent;
    m_stats.bytes_sent += bytes.size();
}

std::size_t Lockstep::submit(std::span<const Replay::Command> commands) {
    FLUX_ASSERT(needs_input(), "Lockstep::submit: ввод на этот момент уже подан (проверяйте needs_input)");
    const std::size_t accepted = std::min(commands.size(), max_commands_per_tick);
    m_inputs[index(m_config.local)][m_local_next] = std::vector<Replay::Command>(commands.begin(), commands.begin() + static_cast<std::ptrdiff_t>(accepted));
    ++m_local_next;
    m_have[index(m_config.local)] = m_local_next;
    return accepted;
}

void Lockstep::make_outgoing(std::uint64_t hash, std::span<const std::byte> bytes, PeerId except) {
    if (std::ranges::any_of(m_outgoing, [hash](const OutgoingBlob& b) { return b.hash == hash; })) return;
    OutgoingBlob out{hash, std::vector<std::byte>(bytes.begin(), bytes.end()), std::vector<bool>(m_config.peers, true), std::vector<std::uint32_t>(m_config.peers, 0)};
    bool any = false;
    for (const PeerId n : m_neighbors) {
        if (n == except) continue;
        out.acked[index(n)] = false;
        any = true;
    }
    if (any) m_outgoing.push_back(std::move(out));
}

std::uint64_t Lockstep::submit_blob(std::span<const std::byte> bytes) {
    FLUX_ASSERT(!bytes.empty() && bytes.size() <= max_blob_size, "Lockstep::submit_blob: размер блоба вне 1…64 КиБ");
    const std::uint64_t hash = Math::content_hash(bytes);
    if (m_blobs.size() < m_config.max_stored_blobs || m_blobs.contains(hash)) m_blobs.emplace(hash, std::vector<std::byte>(bytes.begin(), bytes.end()));
    make_outgoing(hash, bytes, no_peer);
    return hash;
}

bool Lockstep::ready() const {
    if (!connected()) return false;
    for (std::size_t p = 0; p < m_config.peers; ++p) {
        const auto it = m_inputs[p].find(m_tick);
        if (it == m_inputs[p].end()) return false;
        if (!m_blob_reference) continue;
        for (const Replay::Command& c : it->second) {
            if (const auto need = m_blob_reference(c); need && !m_blobs.contains(*need)) return false;
        }
    }
    return true;
}

std::uint32_t Lockstep::pumps_since_heard(PeerId peer) const noexcept {
    if (peer >= m_config.peers || peer == m_config.local || !m_is_neighbor[peer]) return 0;
    return m_pumps - m_last_heard[index(peer)];
}

std::vector<PeerId> Lockstep::silent_peers(std::uint32_t threshold_pumps) const {
    std::vector<PeerId> out;
    for (PeerId p = 0; p < m_config.peers; ++p) {
        if (pumps_since_heard(p) > threshold_pumps) out.push_back(p);
    }
    return out;
}

std::span<const Replay::Command> Lockstep::advance() {
    FLUX_ASSERT(ready(), "Lockstep::advance: не готовы (проверяйте ready)");
    m_merged.clear();
    for (std::size_t p = 0; p < m_config.peers; ++p) {
        const auto it = m_inputs[p].find(m_tick);
        const std::size_t first = m_merged.size();
        m_merged.insert(m_merged.end(), it->second.begin(), it->second.end());
        // Номер игрока — номер пира, приславшего команду: чужую команду за свою не выдать.
        for (std::size_t i = first; i < m_merged.size(); ++i) m_merged[i].player = static_cast<std::uint16_t>(p);
    }
    ++m_tick; // исполненные тики убирает pump: свои и пересылаемые держатся до подтверждения соседями
    return m_merged;
}

void Lockstep::pump() {
    ++m_pumps;
    Datagram d;
    while (m_transport.receive(d)) {
        auto packet = decode(d.bytes);
        if (!packet || packet->from != d.from || packet->from >= m_config.peers || packet->from == m_config.local || !m_is_neighbor[packet->from]) {
            ++m_stats.bad_packets;
            continue;
        }
        ++m_stats.packets_received;
        m_last_heard[index(packet->from)] = m_pumps;
        handle(*packet);
    }

    // Тики игрока держим, пока их не исполнили мы и пока не подтвердили все соседи, кому мы их шлём (иначе нечем лечить потери).
    for (std::size_t j = 0; j < m_config.peers; ++j) {
        std::uint32_t floor = m_tick;
        if (j == index(m_config.local) || m_config.relay) {
            for (const PeerId n : m_neighbors) {
                if (index(n) != j) floor = std::min(floor, m_peer_have[index(n)][j]);
            }
        }
        auto& per = m_inputs[j];
        per.erase(per.begin(), per.lower_bound(floor));
    }

    const bool periodic = m_pumps % m_config.resend_interval == 0;
    if (!connected() && m_failure.empty() && m_pumps % m_config.resend_interval == 1) send_hello();
    else if (m_pumps % hello_period == 0) send_hello();
    if (connected()) {
        for (const PeerId n : m_neighbors) send_inputs(n, periodic);
        send_blobs();
    }
    if (!ready()) ++m_stats.stalled_pumps;
}

void Lockstep::send_hello() {
    for (const PeerId p : m_neighbors) send_packet(p, Hello{m_config.peers, m_config.seed, m_config.config_hash, m_hello_seen[index(p)]});
}

void Lockstep::send_inputs(PeerId to, bool periodic) {
    Inputs msg;
    msg.have = m_have;
    std::size_t size = 2 + 4 * m_have.size(); // счётчики и подтверждения
    const std::size_t first = m_pumps % m_config.peers; // по кругу: ни один игрок не вытесняет остальных из пакета
    for (std::size_t k = 0; k < m_config.peers; ++k) {
        const std::size_t j = (first + k) % m_config.peers;
        if (j == index(to)) continue;                                   // свои команды сосед знает сам
        if (j != index(m_config.local) && !m_config.relay) continue;    // чужие — только при ретрансляции
        PlayerTicks player{static_cast<PeerId>(j), {}};
        size += 3;
        const auto& per = m_inputs[j];
        for (auto it = per.lower_bound(m_peer_have[index(to)][j]); it != per.end() && player.ticks.size() < m_config.max_window; ++it) {
            const std::size_t cost = 5 + it->second.size() * wire_command_size;
            if (size + cost > inputs_budget) break; // остальное уйдёт следующими пакетами, когда подтвердят начало
            size += cost;
            player.ticks.push_back(TickInputs{it->first, it->second});
        }
        if (!player.ticks.empty()) msg.players.push_back(std::move(player));
        else size -= 3;
    }
    if (msg.players.empty() && !periodic && m_sent_have[index(to)] == m_have) return; // нечего сказать
    m_sent_have[index(to)] = m_have;
    send_packet(to, std::move(msg));
}

void Lockstep::send_blobs() {
    for (OutgoingBlob& blob : m_outgoing) {
        for (const PeerId p : m_neighbors) {
            if (blob.acked[index(p)]) continue;
            std::uint32_t& next = blob.next_offset[index(p)];
            for (std::size_t n = 0; n < m_config.max_blob_chunks_per_pump; ++n) {
                if (next >= blob.bytes.size()) next = 0; // дошли до конца — снова с начала, пока не придёт подтверждение
                const std::size_t len = std::min<std::size_t>(blob_chunk_size, blob.bytes.size() - next);
                BlobChunk chunk{blob.hash, static_cast<std::uint32_t>(blob.bytes.size()), next,
                                std::vector<std::byte>(blob.bytes.begin() + next, blob.bytes.begin() + static_cast<std::ptrdiff_t>(next + len))};
                send_packet(p, std::move(chunk));
                next += static_cast<std::uint32_t>(len);
            }
        }
    }
    std::erase_if(m_outgoing, [](const OutgoingBlob& b) { return std::ranges::all_of(b.acked, [](bool a) { return a; }); });
}

void Lockstep::handle(const Packet& packet) {
    const std::size_t from = index(packet.from);
    std::visit([&](const auto& body) {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, Hello>) {
            if (body.peer_count != m_config.peers) m_failure = "пир " + std::to_string(packet.from) + ": другое число игроков";
            else if (body.seed != m_config.seed) m_failure = "пир " + std::to_string(packet.from) + ": другой сид мира";
            else if (body.config_hash != m_config.config_hash) m_failure = "пир " + std::to_string(packet.from) + ": другая настройка игры (уровень или версия правил)";
            else m_hello_seen[from] = true;
            if (m_hello_seen[from] && !body.knows_you) send_packet(packet.from, Hello{m_config.peers, m_config.seed, m_config.config_hash, true}); // он нас ещё не видел: отвечаем (ответ сам ответа не вызывает)
        } else if constexpr (std::is_same_v<T, Inputs>) {
            const std::size_t local = index(m_config.local);
            for (std::size_t j = 0; j < body.have.size() && j < m_config.peers; ++j) {
                const std::uint32_t known = j == local ? m_local_next : m_have[j]; // больше того, что есть у нас, не верим
                m_peer_have[from][j] = std::max(m_peer_have[from][j], std::min(body.have[j], known));
            }
            for (const PlayerTicks& player : body.players) {
                if (player.player >= m_config.peers || index(player.player) == local) continue;
                auto& per = m_inputs[index(player.player)];
                std::uint32_t& have = m_have[index(player.player)];
                for (const TickInputs& t : player.ticks) {
                    if (t.tick < have || t.tick >= m_tick + input_window) continue; // старое или слишком далёкое
                    const auto [it, inserted] = per.try_emplace(t.tick, t.commands);
                    if (!inserted && it->second != t.commands) ++m_stats.input_conflicts;
                }
                while (per.contains(have)) ++have;
            }
        } else if constexpr (std::is_same_v<T, BlobChunk>) {
            if (m_blobs.contains(body.hash)) { send_packet(packet.from, BlobAck{body.hash}); return; } // уже есть: подтверждение потерялось
            if (!m_partial.contains(body.hash)) {
                if (m_partial.size() >= max_partial_blobs || m_blobs.size() >= m_config.max_stored_blobs) return;
                m_partial[body.hash] = Partial{body.total, std::vector<std::byte>(body.total), {}, 0};
            }
            Partial& part = m_partial[body.hash];
            if (part.total != body.total) { m_partial.erase(body.hash); ++m_stats.blobs_rejected; return; }
            if (part.offsets.insert(body.offset).second) {
                std::copy(body.data.begin(), body.data.end(), part.data.begin() + body.offset);
                part.received += static_cast<std::uint32_t>(body.data.size());
            }
            if (part.received >= part.total) {
                if (Math::content_hash(part.data) == body.hash) {
                    const auto stored = m_blobs.emplace(body.hash, std::move(part.data)).first;
                    ++m_stats.blobs_received;
                    send_packet(packet.from, BlobAck{body.hash});
                    if (m_config.relay) make_outgoing(body.hash, stored->second, packet.from); // сервер звезды: остальным клиентам
                } else {
                    ++m_stats.blobs_rejected; // содержимое не сошлось с хешем: отбрасываем, отправитель повторит с начала
                }
                m_partial.erase(body.hash);
            }
        } else if constexpr (std::is_same_v<T, BlobAck>) {
            for (OutgoingBlob& b : m_outgoing) {
                if (b.hash == body.hash) b.acked[from] = true;
            }
        } else if constexpr (std::is_same_v<T, HashReport>) {
            if (const auto mine = m_my_hashes.find(body.tick); mine != m_my_hashes.end()) compare(body.tick, packet.from, body.hashes);
            else if (body.tick + hash_memory > m_tick) m_their_hashes[{body.tick, packet.from}] = body.hashes;
        }
    }, packet.body);
}

void Lockstep::report_hashes(const Replay::StateHashes& hashes) {
    FLUX_ASSERT(m_tick > 0, "Lockstep::report_hashes: ещё не исполнено ни одного тика");
    const std::uint32_t tick = m_tick - 1;
    m_my_hashes[tick] = hashes;
    for (const PeerId p : m_neighbors) {
        send_packet(p, HashReport{tick, hashes});
        if (const auto it = m_their_hashes.find({tick, p}); it != m_their_hashes.end()) {
            compare(tick, p, it->second);
            m_their_hashes.erase(it);
        }
    }
    while (!m_my_hashes.empty() && m_my_hashes.begin()->first + hash_memory < tick) m_my_hashes.erase(m_my_hashes.begin());
    std::erase_if(m_their_hashes, [&](const auto& kv) { return kv.first.first + hash_memory < tick; });
}

void Lockstep::compare(std::uint32_t tick, PeerId peer, const Replay::StateHashes& theirs) {
    if (m_desync) return; // первое расхождение — самое важное: дальше всё разойдётся
    const auto it = m_my_hashes.find(tick);
    if (it == m_my_hashes.end()) return;
    if (auto diff = it->second.differing(theirs); !diff.empty()) m_desync = Desync{tick, peer, std::move(diff)};
}

const std::vector<std::byte>* Lockstep::blob(std::uint64_t hash) const {
    const auto it = m_blobs.find(hash);
    return it == m_blobs.end() ? nullptr : &it->second;
}

} // namespace Net
