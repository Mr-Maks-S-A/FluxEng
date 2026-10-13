#pragma once
/**
 * @file Udp.hpp
 * @brief Настоящий UDP: `Transport` поверх сокета (IPv4, неблокирующий), без изменений протокола `Lockstep`.
 *
 * Протокол уже рассчитан на ненадёжную сеть (потери, повторы, перестановка, порча — `SimulatedNetwork` гоняет всё это
 * в тестах), поэтому сокет подключается тем же интерфейсом `Transport`. Узел знает адреса пиров по номерам;
 * датаграмма от неизвестного адреса отбрасывается и считается (`stats().unknown_sender`) — посторонний не вставит
 * команду «от имени» игрока. Номер отправителя внутри пакета дополнительно сверяет `Lockstep`.
 *
 * @code
 * auto a = Net::UdpTransport::open(0, {.bind_port = 0});                  // порт назначит система
 * auto b = Net::UdpTransport::open(1, {.bind_port = 0});
 * (*a)->set_peer(1, "127.0.0.1", (*b)->port());
 * (*b)->set_peer(0, "127.0.0.1", (*a)->port());
 * Net::Lockstep net(**a, {.local = 0, .peers = 2});                       // дальше — как с SimulatedNetwork
 * @endcode
 *
 * Платформы: POSIX (Linux, macOS). На Windows `open` возвращает ошибку «не поддерживается» — игра остаётся на
 * `SimulatedNetwork` или своём `Transport` (Steam, WebRTC): интерфейс тот же.
 * Время — настоящее: узел не ждёт и не спит, `receive` возвращает только то, что уже пришло.
 */

#include <Net/Transport.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace Net {

struct UdpOptions {
    std::uint16_t bind_port = 0;            ///< 0 — порт назначает система (узнать — `port()`).
    std::string bind_address = "0.0.0.0";   ///< Локальный адрес IPv4; для тестов — «127.0.0.1».
};

struct UdpStats {
    std::uint64_t sent = 0, received = 0;
    std::uint64_t unknown_sender = 0; ///< Пришло от адреса, не занесённого в таблицу пиров (отброшено).
    std::uint64_t oversized = 0;      ///< Отправка больше `max_packet_size` (отброшена).
    std::uint64_t send_errors = 0;    ///< Системная ошибка отправки (буфер полон и т. п.): для UDP это обычная потеря.
};

class UdpTransport final : public Transport {
public:
    /// @brief Открывает сокет. Ошибка — текст (порт занят, адрес неверен, платформа не поддерживает).
    [[nodiscard]] static std::expected<std::unique_ptr<UdpTransport>, std::string> open(PeerId local, const UdpOptions& options = {});
    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    /// @brief Порт, на котором слушает сокет (после `bind_port = 0` — выданный системой).
    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    /// @brief Адрес пира по номеру (IPv4, «127.0.0.1»). `false` — адрес не разобран или номер вне диапазона.
    bool set_peer(PeerId peer, std::string_view host, std::uint16_t port);
    [[nodiscard]] const UdpStats& stats() const noexcept { return m_stats; }

    [[nodiscard]] PeerId local_id() const noexcept override { return m_local; }
    void send(PeerId to, std::span<const std::byte> bytes) override;
    [[nodiscard]] bool receive(Datagram& out) override;

private:
    struct Impl;
    UdpTransport(PeerId local, std::unique_ptr<Impl> impl, std::uint16_t port);

    PeerId m_local;
    std::unique_ptr<Impl> m_impl;
    std::uint16_t m_port;
    UdpStats m_stats;
};

} // namespace Net
