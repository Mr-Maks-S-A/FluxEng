#pragma once
/**
 * @file BusAdapter.hpp
 * @brief Связка журнала с шиной событий EventSystem: записать события тика в журнал и воспроизвести их из журнала.
 *
 * Только этот заголовок знает про EventSystem (цель `engine::EventLogBus`); сам журнал от шины не зависит.
 * Поддерживаются каналы с раскладкой AoS (по умолчанию). События — те же тривиальные структуры с `event_name`.
 *
 * @code
 * // запись: после advance_tick() читатель видит события прошлого тика
 * EventLog::record(writer, tick, hits_reader);
 * EventLog::record(writer, tick, heals_reader);
 *
 * // воспроизведение: события тика из журнала снова уходят в шину
 * EventLog::Replayer replayer(read_result.records);
 * replayer.begin_tick(tick);
 * replayer.emit(hits_writer);
 * replayer.emit(heals_writer);
 * @endcode
 */

#include <EventLog/Journal.hpp>

#include <EventSystem/EventSystem.hpp>

#include <algorithm>
#include <span>

namespace EventLog {

/// @brief Пишет в журнал все события канала, которые читатель видит в этом тике. Возвращает, сколько записано.
template<LoggableEvent E>
std::size_t record(Writer& writer, std::uint32_t tick, const EventSystem::EventReader<E>& reader) {
    std::size_t count = 0;
    for (const E& event : reader.events()) {
        writer.append(tick, event);
        ++count;
    }
    return count;
}

/// @brief Воспроизводит записанные события: выбирает записи одного тика и отправляет события нужного типа в шину.
/// Записи должны идти по неубывающим тикам (так их и пишет симуляция).
class Replayer {
public:
    explicit Replayer(std::span<const Record> records) : m_records(records) {}

    /// @brief Выбирает записи тика `tick` (порядок записи сохраняется).
    void begin_tick(std::uint32_t tick) {
        const auto lower = std::ranges::lower_bound(m_records, tick, {}, &Record::tick);
        const auto upper = std::ranges::upper_bound(m_records, tick, {}, &Record::tick);
        m_first = static_cast<std::size_t>(lower - m_records.begin());
        m_last = static_cast<std::size_t>(upper - m_records.begin());
    }

    /// @brief Отправляет события типа `E` выбранного тика. Возвращает, сколько отправлено.
    template<LoggableEvent E>
    std::size_t emit(EventSystem::EventWriter<E>& writer) const {
        std::size_t count = 0;
        for (std::size_t i = m_first; i < m_last; ++i) {
            if (const std::optional<E> event = decode<E>(m_records[i])) {
                (void)writer.emit(*event);
                ++count;
            }
        }
        return count;
    }

    /// @brief Сколько записей в выбранном тике (всех типов).
    [[nodiscard]] std::size_t records_in_tick() const noexcept { return m_last - m_first; }

private:
    std::span<const Record> m_records;
    std::size_t m_first = 0, m_last = 0;
};

} // namespace EventLog
