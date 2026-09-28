#pragma once
/**
 * @file EventWriter.hpp
 * @brief Типизированный писатель событий.
 */

#include <EventSystem/Channel/StreamChannel.hpp>
#include <EventSystem/Core/Event.hpp>

#include <cassert>
#include <cstddef>

namespace EventSystem {

/**
 * @brief Отправляет события типа `E` в канал.
 *
 * Лёгкий объект (один указатель), который система получает один раз
 * (EventBus::writer()) и дальше хранит у себя. Поиск канала по ID
 * происходит только при получении писателя, а не на каждый `emit`.
 *
 * @tparam E Тип события.
 * @warning Писатель не владеет каналом и становится невалидным после уничтожения шины.
 */
template<Event E>
class EventWriter {
public:
    /// @brief Невалидный писатель (для отложенной инициализации членов класса).
    EventWriter() noexcept = default;

    /// @brief Писатель канала. Обычно создаётся через EventBus::writer().
    explicit EventWriter(StreamChannel& channel) noexcept : m_channel(&channel) {}

    /**
     * @brief Отправляет событие. Оно станет видно читателям в следующем тике.
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

    /// @brief Сколько событий уже отправлено в текущем тике.
    [[nodiscard]] std::size_t pending_count() const noexcept { return m_channel->pending().size(); }

    /// @brief `true`, если писатель привязан к каналу.
    [[nodiscard]] bool valid() const noexcept { return m_channel != nullptr; }

private:
    StreamChannel* m_channel = nullptr;
};

} // namespace EventSystem
