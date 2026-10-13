#include <Net/Udp.hpp>

#if defined(_WIN32)

namespace Net {

struct UdpTransport::Impl {};
UdpTransport::UdpTransport(PeerId local, std::unique_ptr<Impl> impl, std::uint16_t port) : m_local(local), m_impl(std::move(impl)), m_port(port) {}
UdpTransport::~UdpTransport() = default;
std::expected<std::unique_ptr<UdpTransport>, std::string> UdpTransport::open(PeerId, const UdpOptions&) {
    return std::unexpected("UDP-транспорт на Windows не поддерживается: используйте SimulatedNetwork или свой Transport");
}
bool UdpTransport::set_peer(PeerId, std::string_view, std::uint16_t) { return false; }
void UdpTransport::send(PeerId, std::span<const std::byte>) {}
bool UdpTransport::receive(Datagram&) { return false; }

} // namespace Net

#else

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>

namespace Net {

struct UdpTransport::Impl {
    int fd = -1;
    std::array<sockaddr_in, max_peers> peers{};
    std::array<bool, max_peers> known{};
    ~Impl() {
        if (fd >= 0) ::close(fd);
    }
};

UdpTransport::UdpTransport(PeerId local, std::unique_ptr<Impl> impl, std::uint16_t port) : m_local(local), m_impl(std::move(impl)), m_port(port) {}
UdpTransport::~UdpTransport() = default;

std::expected<std::unique_ptr<UdpTransport>, std::string> UdpTransport::open(PeerId local, const UdpOptions& options) {
    if (local >= max_peers) return std::unexpected("UDP: номер узла вне диапазона");
    auto impl = std::make_unique<Impl>();
    impl->fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (impl->fd < 0) return std::unexpected(std::string("UDP: socket: ") + std::strerror(errno));
    const int flags = ::fcntl(impl->fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(impl->fd, F_SETFL, flags | O_NONBLOCK) < 0) return std::unexpected(std::string("UDP: O_NONBLOCK: ") + std::strerror(errno));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.bind_port);
    if (::inet_pton(AF_INET, options.bind_address.c_str(), &address.sin_addr) != 1) return std::unexpected("UDP: неверный адрес: " + options.bind_address);
    if (::bind(impl->fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) < 0) return std::unexpected(std::string("UDP: bind: ") + std::strerror(errno));

    sockaddr_in bound{};
    socklen_t length = sizeof bound;
    if (::getsockname(impl->fd, reinterpret_cast<sockaddr*>(&bound), &length) < 0) return std::unexpected(std::string("UDP: getsockname: ") + std::strerror(errno));
    const std::uint16_t port = ntohs(bound.sin_port);
    return std::unique_ptr<UdpTransport>(new UdpTransport(local, std::move(impl), port));
}

bool UdpTransport::set_peer(PeerId peer, std::string_view host, std::uint16_t port) {
    if (peer >= max_peers) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    const std::string text(host);
    if (::inet_pton(AF_INET, text.c_str(), &address.sin_addr) != 1) return false;
    m_impl->peers[peer] = address;
    m_impl->known[peer] = true;
    return true;
}

void UdpTransport::send(PeerId to, std::span<const std::byte> bytes) {
    if (to >= max_peers || !m_impl->known[to]) return;
    if (bytes.size() > max_packet_size) {
        ++m_stats.oversized;
        return;
    }
    const sockaddr_in& address = m_impl->peers[to];
    const ssize_t n = ::sendto(m_impl->fd, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    if (n < 0) ++m_stats.send_errors;
    else ++m_stats.sent;
}

bool UdpTransport::receive(Datagram& out) {
    std::array<std::byte, max_packet_size + 1> buffer; // +1: датаграмма больше допустимой видна по длине и отбрасывается
    for (;;) {
        sockaddr_in from{};
        socklen_t length = sizeof from;
        const ssize_t n = ::recvfrom(m_impl->fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from), &length);
        if (n < 0) return false; // EAGAIN: пусто; прочие ошибки для UDP — тоже «ничего не пришло»
        PeerId sender = no_peer;
        for (PeerId p = 0; p < max_peers; ++p) {
            if (m_impl->known[p] && m_impl->peers[p].sin_addr.s_addr == from.sin_addr.s_addr && m_impl->peers[p].sin_port == from.sin_port) {
                sender = p;
                break;
            }
        }
        if (sender == no_peer || static_cast<std::size_t>(n) > max_packet_size) {
            ++m_stats.unknown_sender;
            continue; // посторонний или слишком большой: читаем следующую
        }
        ++m_stats.received;
        out.from = sender;
        out.bytes.assign(buffer.begin(), buffer.begin() + n);
        return true;
    }
}

} // namespace Net

#endif
