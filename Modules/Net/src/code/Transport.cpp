#include <Net/Transport.hpp>

#include <Math/Assert.hpp>

#include <algorithm>

namespace Net {

class LoopbackNetwork::Endpoint final : public Transport {
public:
    Endpoint(LoopbackNetwork& net, PeerId id) : m_net(net), m_id(id) {}
    void send(PeerId to, std::span<const std::byte> bytes) override { m_net.deliver(m_id, to, bytes); }
    [[nodiscard]] bool receive(Datagram& out) override {
        if (m_inbox.empty()) return false;
        out = std::move(m_inbox.front());
        m_inbox.pop_front();
        return true;
    }
    std::deque<Datagram> m_inbox;

private:
    LoopbackNetwork& m_net;
    PeerId m_id;
};

LoopbackNetwork::LoopbackNetwork(std::size_t peers, const LinkConfig& link, std::uint64_t seed) : m_link(link), m_rng(seed) {
    for (std::size_t i = 0; i < peers; ++i) m_endpoints.push_back(std::make_unique<Endpoint>(*this, static_cast<PeerId>(i)));
}
LoopbackNetwork::~LoopbackNetwork() = default;

Transport& LoopbackNetwork::endpoint(PeerId peer) {
    FLUX_ASSERT(peer < m_endpoints.size(), "LoopbackNetwork::endpoint: нет такого пира");
    return *m_endpoints[peer];
}

void LoopbackNetwork::deliver(PeerId from, PeerId to, std::span<const std::byte> bytes) {
    ++m_stats.sent;
    m_stats.bytes += bytes.size();
    if (to >= m_endpoints.size()) { ++m_stats.lost; return; }
    if (m_rng.below(1000) < m_link.loss_permille) { ++m_stats.lost; return; }
    const int copies = m_rng.below(1000) < m_link.duplicate_permille ? 2 : 1;
    if (copies == 2) ++m_stats.duplicated;
    for (int i = 0; i < copies; ++i) {
        InFlight f{m_now + m_link.latency_steps + m_rng.below(m_link.jitter_steps + 1), to, Datagram{from, {bytes.begin(), bytes.end()}}};
        if (!f.datagram.bytes.empty() && m_rng.below(1000) < m_link.corrupt_permille) {
            const std::uint32_t bit = m_rng.below(static_cast<std::uint32_t>(f.datagram.bytes.size() * 8));
            f.datagram.bytes[bit / 8] ^= static_cast<std::byte>(1u << (bit % 8));
            ++m_stats.corrupted;
        }
        m_flight.push_back(std::move(f));
    }
}

void LoopbackNetwork::step() {
    ++m_now;
    // Созревшие — в почтовые ящики, в порядке времени созревания (при равенстве — в порядке отправки): перестановка возникает от разброса.
    std::stable_sort(m_flight.begin(), m_flight.end(), [](const InFlight& a, const InFlight& b) { return a.ready_at < b.ready_at; });
    std::size_t done = 0;
    while (done < m_flight.size() && m_flight[done].ready_at <= m_now) {
        m_endpoints[m_flight[done].to]->m_inbox.push_back(std::move(m_flight[done].datagram));
        ++m_stats.delivered;
        ++done;
    }
    m_flight.erase(m_flight.begin(), m_flight.begin() + static_cast<std::ptrdiff_t>(done));
}

} // namespace Net
