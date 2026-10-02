#pragma once
/**
 * @file EventReader.hpp
 * @brief Типизированный читатель событий.
 */

#include <EventSystem/Channel/Channel.hpp>
#include <EventSystem/Core/Event.hpp>

#include <cassert>
#include <cstddef>
#include <span>

namespace EventSystem {

/**
 * @brief Читает события типа `E`, доступные в текущем тике.
 *
 * Как и EventWriter, получается один раз и хранится в системе.
 * Все методы возвращают данные из буфера канала без копирования
 * (кроме get() / for_each() для SoA, где событие собирается из колонок).
 *
 * Выбор метода зависит от раскладки события, и ошибка ловится при компиляции:
 * - Layout::AoS → events(): `std::span<const E>`;
 * - Layout::SoA → column<&E::field>(): `std::span<const T>` одного поля;
 * - любая раскладка → size(), field<&E::field>(i), get(i), for_each().
 *
 * @tparam E Тип события.
 * @warning Данные валидны до ближайшего EventBus::advance_tick().
 */
template<Event E>
class EventReader {
public:
    /// @brief Невалидный читатель (для отложенной инициализации членов класса).
    EventReader() noexcept = default;

    /// @brief Читатель канала. Обычно создаётся через EventBus::reader().
    explicit EventReader(const Channel& channel) noexcept : m_channel(&channel), m_buffer(&channel.ready()) {}

    /// @brief Количество доступных событий.
    [[nodiscard]] std::size_t size() const noexcept { return buffer().size(); }
    /// @brief `true`, если событий нет.
    [[nodiscard]] bool empty() const noexcept { return buffer().empty(); }

    /// @brief Все события непрерывным массивом. Только для Layout::AoS.
    [[nodiscard]] std::span<const E> events() const noexcept { return buffer().template events<E>(); }

    /**
     * @brief Колонка одного поля. Только для Layout::SoA.
     * @tparam Member Поле события, например `&VoxelChanged::block`.
     */
    template<auto Member>
    [[nodiscard]] std::span<const member_value_t<Member>> column() const noexcept {
        return buffer().template column<E, Member>();
    }

    /**
     * @brief Значение поля события `index`.
     * @pre `index < size()`.
     */
    template<auto Member>
    [[nodiscard]] const member_value_t<Member>& field(std::size_t index) const noexcept {
        return buffer().template field<E, Member>(index);
    }

    /**
     * @brief Копия события `index`.
     * @pre `index < size()`.
     */
    [[nodiscard]] E get(std::size_t index) const noexcept { return buffer().template get<E>(index); }

    /**
     * @brief Вызывает `fn(const E&)` для каждого события.
     *
     * Удобно, когда нужны все поля. Для SoA каждое событие собирается из колонок;
     * если нужна только часть полей, быстрее column().
     */
    template<typename Fn>
    void for_each(Fn&& fn) const {
        if constexpr (event_layout_v<E> == Layout::AoS) {
            for (const E& event : events()) {
                fn(event);
            }
        } else {
            const std::size_t count = size();
            for (std::size_t i = 0; i < count; ++i) {
                const E event = get(i);
                fn(event);
            }
        }
    }

    /**
     * @brief Ссылка на событие `index` — передайте её в EventWriter::emit(e, cause), чтобы связать следствие с причиной.
     * Одинакова при любом числе потоков (вычисляется из канала, момента и номера).
     */
    [[nodiscard]] EventRef ref(std::size_t index) const noexcept { return m_channel->ref(index); }

    /// @brief Причина события `index` (если у канала включена трассировка), иначе нулевая ссылка.
    [[nodiscard]] EventRef cause(std::size_t index) const noexcept { return buffer().cause(index); }

    /// @brief `true`, если читатель привязан к каналу.
    [[nodiscard]] bool valid() const noexcept { return m_buffer != nullptr; }

private:
    [[nodiscard]] const EventBuffer& buffer() const noexcept {
        assert(valid() && "EventReader is not bound to a channel");
        return *m_buffer;
    }

    const Channel* m_channel = nullptr;
    const EventBuffer* m_buffer = nullptr;
};

} // namespace EventSystem
