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
      m_inputs(config.peers), m_have(config.peers, config.input_delay), m_peer_acked(config.peers, config.input_delay),
      m_hello_seen(config.peers, false) {
    FLUX_ASSERT(config.peers >= 1 && config.local < config.peers, "Lockstep: локальный пир вне 0…peers-1");
    FLUX_ASSERT(config.input_delay >= 1, "Lockstep: задержка ввода должна быть ≥ 1 тика");
    m_hello_seen[config.local] = true;
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

std::uint64_t Lockstep::submit_blob(std::span<const std::byte> bytes) {
    FLUX_ASSERT(!bytes.empty() && bytes.size() <= max_blob_size, "Lockstep::submit_blob: размер блоба вне 1…64 КиБ");
    const std::uint64_t hash = Math::content_hash(bytes);
    if (m_blobs.size() < m_config.max_stored_blobs || m_blobs.contains(hash)) m_blobs.emplace(hash, std::vector<std::byte>(bytes.begin(), bytes.end()));
    const bool known = std::ranges::any_of(m_outgoing, [hash](const OutgoingBlob& b) { return b.hash == hash; });
    if (!known && m_config.peers > 1) {
        OutgoingBlob out{hash, std::vector<std::byte>(bytes.begin(), bytes.end()), std::vector<bool>(m_config.peers, false), std::vector<std::uint32_t>(m_config.peers, 0)};
        out.acked[index(m_config.local)] = true;
        m_outgoing.push_back(std::move(out));
    }
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

std::span<const Replay::Command> Lockstep::advance() {
    FLUX_ASSERT(ready(), "Lockstep::advance: не готовы (проверяйте ready)");
    m_merged.clear();
    for (std::size_t p = 0; p < m_config.peers; ++p) {
        auto& per_peer = m_inputs[p];
        const auto it = per_peer.find(m_tick);
        m_merged.insert(m_merged.end(), it->second.begin(), it->second.end());
        if (p != index(m_config.local)) per_peer.erase(it); // свои держим до подтверждения всеми (для повторной отправки)
    }
    ++m_tick;
    return m_merged;
}

void Lockstep::pump() {
    ++m_pumps;
    Datagram d;
    while (m_transport.receive(d)) {
        auto packet = decode(d.bytes);
        if (!packet || packet->from != d.from || packet->from >= m_config.peers || packet->from == m_config.local) {
            ++m_stats.bad_packets;
            continue;
        }
        ++m_stats.packets_received;
        handle(*packet);
    }

    // Свои тики держим, пока их не подтвердили все (иначе нечем лечить потери) и пока они не исполнены.
    std::uint32_t floor = m_tick;
    for (std::size_t p = 0; p < m_config.peers; ++p) {
        if (p != index(m_config.local)) floor = std::min(floor, m_peer_acked[p]);
    }
    auto& mine = m_inputs[index(m_config.local)];
    mine.erase(mine.begin(), mine.lower_bound(floor));

    if (!connected() && m_failure.empty() && m_pumps % m_config.resend_interval == 1) send_hello();
    else if (m_pumps % hello_period == 0) send_hello();
    if (connected()) {
        for (PeerId p = 0; p < m_config.peers; ++p) {
            if (p != m_config.local) send_inputs(p);
        }
        send_blobs();
    }
    if (!ready()) ++m_stats.stalled_pumps;
}

void Lockstep::send_hello() {
    for (PeerId p = 0; p < m_config.peers; ++p) {
        if (p != m_config.local) send_packet(p, Hello{m_config.peers, m_config.seed, m_config.config_hash, m_hello_seen[index(p)]});
    }
}

void Lockstep::send_inputs(PeerId to) {
    Inputs msg;
    msg.acked = m_have[index(to)];
    const auto& mine = m_inputs[index(m_config.local)];
    std::size_t size = 0;
    for (auto it = mine.lower_bound(m_peer_acked[index(to)]); it != mine.end(); ++it) {
        const std::size_t cost = 5 + it->second.size() * sizeof(Replay::Command);
        if (size + cost > inputs_budget) break; // остальное уйдёт следующими пакетами, когда подтвердят начало
        size += cost;
        msg.ticks.push_back(TickInputs{it->first, it->second});
    }
    send_packet(to, std::move(msg));
}

void Lockstep::send_blobs() {
    for (OutgoingBlob& blob : m_outgoing) {
        for (PeerId p = 0; p < m_config.peers; ++p) {
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
            m_peer_acked[from] = std::max(m_peer_acked[from], std::min(body.acked, m_local_next));
            for (const TickInputs& t : body.ticks) {
                if (t.tick < m_have[from] || t.tick >= m_tick + input_window) continue; // старое или слишком далёкое
                m_inputs[from].try_emplace(t.tick, t.commands);
            }
            while (m_inputs[from].contains(m_have[from])) ++m_have[from];
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
                    m_blobs.emplace(body.hash, std::move(part.data));
                    ++m_stats.blobs_received;
                    send_packet(packet.from, BlobAck{body.hash});
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
    for (PeerId p = 0; p < m_config.peers; ++p) {
        if (p == m_config.local) continue;
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
