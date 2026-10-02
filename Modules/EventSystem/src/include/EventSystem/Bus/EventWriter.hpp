#pragma once
/**
 * @file EventWriter.hpp
 * @brief Типизированный писатель событий и писатель дорожек для параллельных систем.
 */

#include <EventSystem/Channel/Channel.hpp>
#include <EventSystem/Core/Event.hpp>
#include <EventSystem/Core/Ids.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace EventSystem {

/**
 * @brief Пишет события `E` из многих потоков без блокировок: по дорожке на кусок работы.
 *
 * @code
 * auto lanes = writer.lanes(JobSystem::chunk_count(count, grain));   // главный поток, до работы
 * JobSystem::parallel_for(jobs, count, grain, [&](std::size_t b, std::size_t e, std::size_t chunk) {
 *     for (std::size_t i = b; i < e; ++i)
 *         if (hit(i)) lanes.emit(chunk, Hit{...});                     // без мьютексов и атомиков
 * });
 * // при смене тика дорожки сольются в порядке номеров: результат одинаков при любом числе потоков
 * @endcode
 *
 * Дорожка — обычный EventBuffer той же схемы и раскладки: у SoA-события в ней те же колонки,
 * а слияние копирует каждую колонку одним memcpy.
 *
 * Правила: одну дорожку пишет один поток; дорожки живут до ближайшей смены момента канала;
 * бюджет канала (max_events_per_tick) применяется при слиянии — лишнее отбрасывается с конца.
 */
template<Event E>
class LaneWriter {
public:
    LaneWriter() noexcept = default;
    LaneWriter(Channel& channel, std::uint32_t base, std::uint32_t count) noexcept
        : m_channel(&channel), m_base(base), m_count(count) {}

    /// @brief Событие в дорожку `lane` (0…size()-1).
    void emit(std::size_t lane, const E& event) {
        assert(lane < m_count && "LaneWriter::emit: lane out of range");
        m_channel->lane(m_base + static_cast<std::uint32_t>(lane)).push(event);
    }

    /// @brief Событие с причиной (дерево причин, ChannelConfig::trace).
    void emit(std::size_t lane, const E& event, EventRef cause) {
        EventBuffer& buffer = m_channel->lane(m_base + static_cast<std::uint32_t>(lane));
        buffer.push(event);
        buffer.set_last_cause(cause);
    }

    /// @brief Сколько дорожек.
    [[nodiscard]] std::size_t size() const noexcept { return m_count; }
    /// @brief Всего событий во всех дорожках (вызывать после параллельной работы, до смены момента).
    [[nodiscard]] std::size_t total() const noexcept {
        std::size_t n = 0;
        for (std::uint32_t i = 0; i < m_count; ++i) n += m_channel->lane(m_base + i).size();
        return n;
    }
    /// @brief Событий уже записано в дорожку (читать — только её потоку или после работы).
    [[nodiscard]] std::size_t lane_size(std::size_t lane) const noexcept {
        return m_channel->lane(m_base + static_cast<std::uint32_t>(lane)).size();
    }

private:
    Channel* m_channel = nullptr;
    std::uint32_t m_base = 0;
    std::uint32_t m_count = 0;
};

/**
 * @brief Отправляет события типа `E` в канал.
 *
 * Лёгкий объект (один указатель), который система получает один раз
 * (EventBus::writer()) и дальше хранит у себя. Поиск канала по ID
 * происходит только при получении писателя, а не на каждый `emit`.
 * Работает с каналом любой политики и домена: политика влияет только на смену момента.
 *
 * @tparam E Тип события.
 * @warning Писатель не владеет каналом и становится невалидным после уничтожения шины.
 * @note emit() — из одного потока; для параллельной записи — lanes().
 */
template<Event E>
class EventWriter {
public:
    /// @brief Невалидный писатель (для отложенной инициализации членов класса).
    EventWriter() noexcept = default;

    /// @brief Писатель канала. Обычно создаётся через EventBus::writer().
    explicit EventWriter(Channel& channel) noexcept : m_channel(&channel) {}

    /**
     * @brief Отправляет событие. Оно станет видно читателям в следующем моменте (тике или кадре).
     * @return `false`, если событие отброшено из-за бюджета канала (ChannelConfig::max_events_per_tick).
     */
    bool emit(const E& event) {
        assert(valid() && "EventWriter is not bound to a channel");
        if (!m_channel->has_room()) [[unlikely]] {
            m_channel->note_dropped();
            return false;
        }
        m_channel->pending().push(event);
        return true;
    }

    /**
     * @brief Отправляет событие с причиной — ссылкой на событие, которое его вызвало (EventReader::ref()).
     * Причины хранятся, если у канала включён ChannelConfig::trace; иначе это обычный emit.
     */
    bool emit(const E& event, EventRef cause) {
        if (!emit(event)) return false;
        m_channel->pending().set_last_cause(cause);
        return true;
    }

    /**
     * @brief Scheduled: событие станет видно через `delay` моментов (тиков или кадров канала).
     * @pre Канал создан с Delivery::Scheduled.
     */
    void emit_after(const E& event, Tick delay, EventRef cause = {}) { m_channel->schedule(event, delay, cause); }

    /**
     * @brief Дорожки для параллельной записи (см. LaneWriter). Вызывать из главного потока до работы.
     * @param count Сколько дорожек — обычно число кусков parallel_for.
     */
    [[nodiscard]] LaneWriter<E> lanes(std::size_t count) {
        const auto n = static_cast<std::uint32_t>(count);
        return LaneWriter<E>(*m_channel, m_channel->open_lanes(n), n);
    }

    /// @brief Сколько событий уже отправлено в текущем моменте (без дорожек).
    [[nodiscard]] std::size_t pending_count() const noexcept { return m_channel->pending().size(); }

    /// @brief `true`, если писатель привязан к каналу.
    [[nodiscard]] bool valid() const noexcept { return m_channel != nullptr; }

private:
    Channel* m_channel = nullptr;
};

} // namespace EventSystem
