#pragma once
/**
 * @file SimulatedNetwork.hpp
 * @brief Имитация сети внутри процесса: задержка, джиттер (отсюда и перестановки), потери, дубликаты.
 *
 * Время — виртуальное: сеть не смотрит на часы, его двигает вызывающий (`advance_to`). Поэтому прогон воспроизводим
 * при заданном seed: тот же порядок вызовов даёт те же потери и тот же порядок доставки. Генератор случайных
 * чисел — собственный (PCG32), а не `<random>`, чьи распределения отличаются между стандартными библиотеками.
 *
 * @code
 * NetSystem::SimulatedNetwork net(42, {.latency_us = 40'000, .jitter_us = 15'000, .loss = 0.05});
 * NetSystem::ITransport& a = net.add_endpoint();   // PeerId 0
 * NetSystem::ITransport& b = net.add_endpoint();   // PeerId 1
 * a.send(1, bytes);
 * net.advance_to(100'000);                          // 100 мс виртуального времени
 * NetSystem::Packet p; while (b.receive(p)) { … }
 * @endcode
 */

#include <NetSystem/Transport.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace NetSystem {

/// @brief Параметры одного направления связи.
struct LinkConfig {
    std::uint64_t latency_us = 30'000; ///< Базовая задержка в микросекундах.
    std::uint64_t jitter_us = 0;       ///< Добавка 0…jitter_us (равномерно): пакеты обгоняют друг друга.
    double loss = 0.0;                 ///< Вероятность потери пакета, 0…1.
    double duplicate = 0.0;            ///< Вероятность доставить пакет дважды, 0…1.
};

struct NetworkStats {
    std::uint64_t sent = 0;       ///< Вызовов send().
    std::uint64_t delivered = 0;  ///< Датаграмм, попавших в очереди приёма (с дубликатами).
    std::uint64_t dropped = 0;    ///< Потеряно (loss, неизвестный адресат).
    std::uint64_t duplicated = 0; ///< Лишних копий.
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
    ITransport& add_endpoint();

    /// @brief Параметры связи по умолчанию (для всех направлений без своих).
    void set_default_link(const LinkConfig& link) noexcept { m_default = link; }
    /// @brief Параметры одного направления `from → to`.
    void set_link(PeerId from, PeerId to, const LinkConfig& link) { m_links[{from, to}] = link; }

    /// @brief Продвинуть виртуальное время и доставить всё, что созрело. Время не идёт назад.
    void advance_to(std::uint64_t now_us);

    [[nodiscard]] std::uint64_t now_us() const noexcept { return m_now; }
    [[nodiscard]] std::size_t in_flight() const noexcept { return m_in_flight.size(); }
    [[nodiscard]] const NetworkStats& stats() const noexcept { return m_stats; }

private:
    class Endpoint;
    struct InFlight {
        PeerId from = kNoPeer;
        PeerId to = kNoPeer;
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

} // namespace NetSystem
