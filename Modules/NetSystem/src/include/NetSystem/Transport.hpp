#pragma once
/**
 * @file Transport.hpp
 * @brief Абстракция транспорта: датаграммы между узлами. Ненадёжные, неупорядоченные, могут дублироваться —
 * как UDP. Надёжность и порядок строит протокол сверху (Lockstep).
 *
 * Реализации: SimulatedNetwork (в процессе, с задержкой/потерями — для тестов и проверки детерминизма).
 * Сокеты (UDP/Steam/WebRTC) подключаются тем же интерфейсом и ничего в протоколе не меняют.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace NetSystem {

using PeerId = std::uint16_t;
inline constexpr PeerId kNoPeer = 0xFFFF;

/// @brief Предел размера датаграммы: безопасно влезает в MTU без фрагментации (1200 + заголовки UDP/IP/туннеля).
inline constexpr std::size_t kMaxPacketBytes = 1200;

struct Packet {
    PeerId from = kNoPeer;
    std::vector<std::byte> data;
};

class ITransport {
public:
    virtual ~ITransport() = default;
    [[nodiscard]] virtual PeerId local_id() const noexcept = 0;
    /// @brief Отправить датаграмму. Никаких гарантий доставки. Больше kMaxPacketBytes — отбрасывается.
    virtual void send(PeerId to, std::span<const std::byte> data) = 0;
    /// @brief Взять следующую принятую датаграмму. false — очередь пуста.
    [[nodiscard]] virtual bool receive(Packet& out) = 0;
};

} // namespace NetSystem
