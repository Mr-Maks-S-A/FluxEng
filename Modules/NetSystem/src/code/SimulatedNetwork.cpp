#include <NetSystem/SimulatedNetwork.hpp>

#include <algorithm>
#include <deque>

namespace NetSystem {

class SimulatedNetwork::Endpoint final : public ITransport {
public:
    Endpoint(SimulatedNetwork& network, PeerId id) noexcept : m_network(network), m_id(id) {}

    [[nodiscard]] PeerId local_id() const noexcept override { return m_id; }
    void send(PeerId to, std::span<const std::byte> data) override { m_network.enqueue(m_id, to, data); }
    [[nodiscard]] bool receive(Packet& out) override {
        if (m_inbox.empty()) return false;
        out = std::move(m_inbox.front());
        m_inbox.pop_front();
        return true;
    }
    void deliver(Packet packet) { m_inbox.push_back(std::move(packet)); }

private:
    SimulatedNetwork& m_network;
    PeerId m_id;
    std::deque<Packet> m_inbox;
};

SimulatedNetwork::SimulatedNetwork(std::uint64_t seed, LinkConfig default_link)
    : m_state(seed * 6364136223846793005ull + 1442695040888963407ull), m_default(default_link) {}

SimulatedNetwork::~SimulatedNetwork() = default;

ITransport& SimulatedNetwork::add_endpoint() {
    m_endpoints.push_back(std::make_unique<Endpoint>(*this, static_cast<PeerId>(m_endpoints.size())));
    return *m_endpoints.back();
}

// PCG32 (XSH-RR): одинаков на всех платформах и стандартных библиотеках.
std::uint32_t SimulatedNetwork::next_u32() noexcept {
    const std::uint64_t old = m_state;
    m_state = old * 6364136223846793005ull + 1442695040888963407ull;
    const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    const auto rot = static_cast<std::uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
}

double SimulatedNetwork::next_unit() noexcept { return static_cast<double>(next_u32()) / 4294967296.0; }

void SimulatedNetwork::enqueue(PeerId from, PeerId to, std::span<const std::byte> data) {
    ++m_stats.sent;
    m_stats.bytes_sent += data.size();
    if (data.size() > kMaxPacketBytes) {
        ++m_stats.oversized;
        return;
    }
    if (to >= m_endpoints.size()) {
        ++m_stats.dropped;
        return;
    }
    const auto link_it = m_links.find({from, to});
    const LinkConfig& link = link_it != m_links.end() ? link_it->second : m_default;

    // Случайные числа берутся в фиксированном порядке и всегда, независимо от исхода: поток не «съезжает» от настроек.
    const double loss_roll = next_unit();
    const double dup_roll = next_unit();
    const std::uint64_t jitter_a = link.jitter_us == 0 ? 0 : next_u32() % (link.jitter_us + 1);
    const std::uint64_t jitter_b = link.jitter_us == 0 ? 0 : next_u32() % (link.jitter_us + 1);

    if (loss_roll < link.loss) {
        ++m_stats.dropped;
        return;
    }
    const auto copies = dup_roll < link.duplicate ? 2 : 1;
    for (int copy = 0; copy < copies; ++copy) {
        const std::uint64_t at = m_now + link.latency_us + (copy == 0 ? jitter_a : jitter_b);
        m_in_flight.emplace(std::pair{at, m_seq++}, InFlight{from, to, std::vector<std::byte>(data.begin(), data.end())});
        if (copy == 1) ++m_stats.duplicated;
    }
}

void SimulatedNetwork::advance_to(std::uint64_t now_us) {
    m_now = std::max(m_now, now_us);
    while (!m_in_flight.empty() && m_in_flight.begin()->first.first <= m_now) {
        auto node = m_in_flight.extract(m_in_flight.begin());
        InFlight& packet = node.mapped();
        m_endpoints[packet.to]->deliver(Packet{packet.from, std::move(packet.data)});
        ++m_stats.delivered;
    }
}

} // namespace NetSystem
