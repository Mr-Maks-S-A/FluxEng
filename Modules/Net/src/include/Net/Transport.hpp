#pragma once
/**
 * @file Transport.hpp
 * @brief Транспорт датаграмм и «плохая сеть» в памяти: задержка, потери, повторы, порча, перестановка — по сиду, воспроизводимо.
 *
 * `Transport` — всё, что нужно протоколу от сети: отправить датаграмму пиру и забрать пришедшие. Датаграммы ненадёжные,
 * неупорядоченные и могут дублироваться, как UDP; надёжность и порядок строит протокол сверху (`Lockstep`).
 * Протокол не знает про сокеты: настоящий UDP, Steam или WebRTC подключаются тем же интерфейсом и ничего в протоколе
 * не меняют, а тесты, примеры и песочницы гоняют `SimulatedNetwork` — ту же сеть в памяти, но с намеренными неполадками.
 *
 * **Время сети виртуальное.** Сеть не смотрит на часы: её двигает вызывающий (`advance_to`, `advance_by`). Прогон
 * воспроизводим при заданном seed: тот же порядок вызовов даёт те же потери и тот же порядок доставки. Генератор
 * случайных чисел собственный (PCG32), а не `<random>`, чьи распределения различаются между стандартными библиотеками.
 *
 * ```
 *   SimulatedNetwork net(seed, {.latency_us = 40'000, .jitter_us = 15'000, .loss = 0.05});
 *   Transport& a = net.add_endpoint();   // PeerId 0
 *   Transport& b = net.add_endpoint();   // PeerId 1
 *   Lockstep la(a, {.local = 0, .peers = 2}), lb(b, {.local = 1, .peers = 2});
 *   каждый кадр: la.pump(); lb.pump(); … net.advance_by(16'667);
 * ```
 */

#include <Net/Wire.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Net {

/// @brief «Нет пира»: отправитель неизвестен.
inline constexpr PeerId no_peer = 0xFF;

struct Datagram {
    PeerId from = no_peer;
    std::vector<std::byte> bytes;
};

class Transport {
public:
    virtual ~Transport() = default;
    /// @brief Номер этого узла.
    [[nodiscard]] virtual PeerId local_id() const noexcept = 0;
    /// @brief Отправить датаграмму пиру. Доставка не гарантируется; больше `max_packet_size` — отбрасывается.
    virtual void send(PeerId to, std::span<const std::byte> bytes) = 0;
    /// @brief Забрать следующую пришедшую датаграмму; `false` — очередь пуста.
    [[nodiscard]] virtual bool receive(Datagram& out) = 0;
};

/// @brief Свойства одного направления связи.
struct LinkConfig {
    std::uint64_t latency_us = 0; ///< Базовая задержка, микросекунды.
    std::uint64_t jitter_us = 0;  ///< Добавка 0…jitter_us (равномерно): пакеты обгоняют друг друга.
    double loss = 0.0;            ///< Вероятность потери пакета, 0…1.
    double duplicate = 0.0;       ///< Вероятность доставить пакет дважды, 0…1.
    double corrupt = 0.0;         ///< Вероятность испортить один бит (контрольная сумма должна поймать), 0…1.
};

struct NetworkStats {
    std::uint64_t sent = 0;       ///< Вызовов send().
    std::uint64_t delivered = 0;  ///< Датаграмм, попавших в очереди приёма (с дубликатами).
    std::uint64_t dropped = 0;    ///< Потеряно (loss, неизвестный адресат).
    std::uint64_t duplicated = 0; ///< Лишних копий.
    std::uint64_t corrupted = 0;  ///< Копий с испорченным битом.
    std::uint64_t oversized = 0;  ///< Отброшено за размер.
    std::uint64_t bytes_sent = 0;
};

class SimulatedNetwork {
public:
    explicit SimulatedNetwork(std::uint64_t seed = 1, LinkConfig default_link = {});
    ~SimulatedNetwork();
    SimulatedNetwork(const SimulatedNetwork&) = delete;
    SimulatedNetwork& operator=(const SimulatedNetwork&) = delete;

    /// @brief Новый узел. PeerId выдаются подряд с нуля. Ссылка живёт до уничтожения сети.
    Transport& add_endpoint();
    /// @brief Узел по номеру (уже добавленный).
    [[nodiscard]] Transport& endpoint(PeerId peer);

    /// @brief Параметры связи по умолчанию (для всех направлений без своих). Можно менять на лету (обрыв связи).
    void set_default_link(const LinkConfig& link) noexcept { m_default = link; }
    /// @brief Параметры одного направления `from → to`.
    void set_link(PeerId from, PeerId to, const LinkConfig& link) { m_links[{from, to}] = link; }

    /// @brief Продвинуть виртуальное время и доставить всё, что созрело. Время не идёт назад.
    void advance_to(std::uint64_t now_us);
    void advance_by(std::uint64_t delta_us) { advance_to(m_now + delta_us); }

    [[nodiscard]] std::uint64_t now_us() const noexcept { return m_now; }
    [[nodiscard]] std::size_t in_flight() const noexcept { return m_in_flight.size(); }
    [[nodiscard]] const NetworkStats& stats() const noexcept { return m_stats; }

private:
    class Endpoint;
    struct InFlight {
        PeerId from = no_peer;
        PeerId to = no_peer;
        std::vector<std::byte> data;
    };

    void enqueue(PeerId from, PeerId to, std::span<const std::byte> data);
    [[nodiscard]] std::uint32_t next_u32() noexcept;
    [[nodiscard]] double next_unit() noexcept; // [0, 1)

    std::uint64_t m_state;
    std::uint64_t m_now = 0;
    std::uint64_t m_seq = 0;
    LinkConfig m_default;
    std::map<std::pair<PeerId, PeerId>, LinkConfig> m_links;
    std::vector<std::unique_ptr<Endpoint>> m_endpoints;
    std::map<std::pair<std::uint64_t, std::uint64_t>, InFlight> m_in_flight; // (время доставки, порядковый номер)
    NetworkStats m_stats;
};

} // namespace Net
