#pragma once
/**
 * @file Transport.hpp
 * @brief Транспорт датаграмм и «плохая сеть» в памяти: задержка, потери, повторы, порча, перестановка — по сиду, воспроизводимо.
 *
 * `Transport` — всё, что нужно протоколу от сети: отправить датаграмму пиру и забрать пришедшие. Протокол (`Lockstep`)
 * не знает про сокеты: настоящий UDP подключается четырьмя десятками строк, а тесты и примеры гоняют `LoopbackNetwork` —
 * ту же сеть в памяти, но с намеренными неполадками. Время сети — шаги (`step()`), обычно один шаг на тик игры.
 *
 * ```
 *   LoopbackNetwork net(2, {.latency_steps = 3, .loss_permille = 150}, seed);
 *   Lockstep a(net.endpoint(0), …), b(net.endpoint(1), …);
 *   каждый тик: a.pump(); b.pump(); … net.step();
 * ```
 */

#include <Net/Wire.hpp>

#include <Math/Rng.hpp>

#include <deque>
#include <memory>
#include <vector>

namespace Net {

struct Datagram {
    PeerId from = 0;
    std::vector<std::byte> bytes;
};

class Transport {
public:
    virtual ~Transport() = default;
    /// @brief Отправить датаграмму пиру (доставка не гарантируется).
    virtual void send(PeerId to, std::span<const std::byte> bytes) = 0;
    /// @brief Забрать следующую пришедшую датаграмму; `false` — очередь пуста.
    [[nodiscard]] virtual bool receive(Datagram& out) = 0;
};

/// @brief Свойства линии между любыми двумя пирами.
struct LinkConfig {
    std::uint32_t latency_steps = 0;     ///< Базовая задержка в шагах сети.
    std::uint32_t jitter_steps = 0;      ///< Случайная добавка 0…jitter (даёт перестановку пакетов).
    std::uint32_t loss_permille = 0;     ///< Потери, ‰.
    std::uint32_t duplicate_permille = 0;///< Дубликаты, ‰.
    std::uint32_t corrupt_permille = 0;  ///< Порча одного бита, ‰ (контрольная сумма должна поймать).
};

struct LinkStats {
    std::uint64_t sent = 0, delivered = 0, lost = 0, duplicated = 0, corrupted = 0, bytes = 0;
};

class LoopbackNetwork {
public:
    LoopbackNetwork(std::size_t peers, const LinkConfig& link = {}, std::uint64_t seed = 1);
    ~LoopbackNetwork();
    LoopbackNetwork(const LoopbackNetwork&) = delete;
    LoopbackNetwork& operator=(const LoopbackNetwork&) = delete;

    /// @brief Точка подключения пира (живёт, пока жива сеть).
    [[nodiscard]] Transport& endpoint(PeerId peer);
    /// @brief Шаг времени сети: созревшие датаграммы становятся доступны получателям.
    void step();
    /// @brief Меняет свойства линии на лету (например, внезапная потеря связи).
    void set_link(const LinkConfig& link) { m_link = link; }
    [[nodiscard]] const LinkStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] std::uint64_t now() const noexcept { return m_now; }

private:
    class Endpoint;
    struct InFlight {
        std::uint64_t ready_at = 0;
        PeerId to = 0;
        Datagram datagram;
    };
    void deliver(PeerId from, PeerId to, std::span<const std::byte> bytes);

    LinkConfig m_link;
    Math::Rng m_rng;
    std::uint64_t m_now = 0;
    std::vector<std::unique_ptr<Endpoint>> m_endpoints;
    std::vector<InFlight> m_flight;
    LinkStats m_stats;
};

} // namespace Net
