#pragma once
/**
 * @file EventBuffer.hpp
 * @brief Хранилище событий одного типа в раскладке AoS или SoA.
 */

#include <EventSystem/Core/Event.hpp>
#include <EventSystem/Core/Ids.hpp>
#include <EventSystem/Core/Schema.hpp>
#include <EventSystem/Storage/ColumnBuffer.hpp>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace EventSystem {

/**
 * @brief Набор событий одного типа, разложенных по колонкам согласно схеме.
 *
 * Два уровня доступа к одним и тем же данным:
 * - **типизированный** (`push<E>`, `events<E>`, `column<E, &E::field>`, `get<E>`) —
 *   для C++-систем; компилируется в прямые `memcpy` и указатели, без рантайм-разбора схемы;
 * - **сырой** (`push_raw`, `read_raw`, `field_data`) — для скриптов, модов и инструментов,
 *   которые знают только EventSchema.
 *
 * Раскладка:
 * - Layout::AoS — одна колонка, элемент = событие целиком (`stride == schema.size`);
 * - Layout::SoA — по колонке на каждое поле (`stride == field.size`).
 *
 * @warning Буфер хранит указатель на схему; схема должна жить дольше буфера
 *          (в шине её владельцем является канал).
 * @note Типизированные методы требуют, чтобы `E` соответствовал схеме буфера.
 *       Шина проверяет это один раз при выдаче писателя/читателя; внутри — только `assert`.
 */
class EventBuffer {
public:
    /// @brief Начальная вместимость при первом росте без явного reserve().
    static constexpr std::size_t initial_capacity = 64;

    /// @brief Создаёт пустой буфер под схему (память не выделяется).
    explicit EventBuffer(const EventSchema& schema);

    EventBuffer(const EventBuffer&) = delete;
    EventBuffer& operator=(const EventBuffer&) = delete;
    EventBuffer(EventBuffer&&) noexcept = default;
    EventBuffer& operator=(EventBuffer&&) noexcept = default;

    /// @brief Обменивает содержимое двух буферов одной схемы за O(1), без аллокаций.
    void swap(EventBuffer& other) noexcept;

    /// @brief Схема, по которой разложены данные.
    [[nodiscard]] const EventSchema& schema() const noexcept { return *m_schema; }
    /// @brief Раскладка данных.
    [[nodiscard]] Layout layout() const noexcept { return m_schema->layout; }
    /// @brief Количество событий.
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    /// @brief `true`, если событий нет.
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    /// @brief Вместимость без перевыделения.
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    /// @brief Суммарно выделено памяти всеми колонками, байт.
    [[nodiscard]] std::size_t allocated_bytes() const noexcept;

    /// @brief Количество колонок (1 для AoS, число полей для SoA).
    [[nodiscard]] std::size_t column_count() const noexcept { return m_columns.size(); }
    /// @brief Колонка по индексу (без проверки границ).
    [[nodiscard]] const ColumnBuffer& column_buffer(std::size_t index) const noexcept { return m_columns[index]; }

    /**
     * @brief Гарантирует место под `capacity` событий.
     * @throws std::bad_alloc При нехватке памяти (буфер остаётся валидным).
     */
    void reserve(std::size_t capacity);

    /// @brief Удаляет все события; память сохраняется для переиспользования.
    void clear() noexcept {
        m_size = 0;
        m_causes.clear();
    }

    // ------------------------------------------------------------------ строки и причины

    /**
     * @brief Дописывает события `[first, first + count)` из `source` (та же схема): по memcpy на колонку.
     *
     * Так сливаются дорожки потоков и переносятся отложенные события — SoA остаётся SoA,
     * каждая колонка копируется одним непрерывным блоком.
     */
    void append(const EventBuffer& source, std::size_t first, std::size_t count);
    /// @brief Заменяет событие `index` событием `source_index` из `source` (слияние Coalesced).
    void overwrite(std::size_t index, const EventBuffer& source, std::size_t source_index) noexcept;
    /// @brief Удаляет первые `count` событий, остальные сдвигаются в начало (отложенные события).
    void erase_front(std::size_t count) noexcept;

    /// @brief Хранить причину каждого события (дерево причин).
    void set_tracing(bool enabled) {
        m_tracing = enabled;
        if (enabled) {
            m_causes.resize(m_size, 0); // уже записанные события — без причины
        } else {
            m_causes.clear();
        }
    }
    /// @brief Хранятся ли причины.
    [[nodiscard]] bool tracing() const noexcept { return m_tracing; }
    /// @brief Причина события `index` (нулевая, если трассировка выключена или причины нет).
    [[nodiscard]] EventRef cause(std::size_t index) const noexcept {
        return m_tracing && index < m_causes.size() ? EventRef{m_causes[index]} : EventRef{};
    }
    /// @brief Задаёт причину последнего записанного события (используется вместе с push).
    void set_last_cause(EventRef cause) noexcept {
        if (m_tracing && !m_causes.empty()) m_causes.back() = cause.value;
    }

    // ------------------------------------------------------------------ сырой доступ

    /**
     * @brief Добавляет событие из байтового представления в AoS-форме (`schema.size` байт).
     * @param event Указатель на `schema().size` байт; поля берутся по `FieldDesc::offset`.
     */
    void push_raw(const std::byte* event);

    /**
     * @brief Копирует событие `index` в байтовое AoS-представление.
     * @param out Буфер на `schema().size` байт. Для SoA заполняются только байты полей,
     *            padding остаётся как был.
     * @pre `index < size()`.
     */
    void read_raw(std::size_t index, std::byte* out) const noexcept;

    /**
     * @brief Указатель на значение поля `field` у события `index` — независимо от раскладки.
     * @pre `index < size()`, `field < schema().fields.size()`.
     */
    [[nodiscard]] const std::byte* field_data(std::size_t index, std::size_t field) const noexcept;

    // ------------------------------------------------------------------ типизированный доступ

    /**
     * @brief Добавляет событие.
     * @tparam E Тип события, соответствующий схеме буфера.
     */
    template<Event E>
    void push(const E& event);

    /**
     * @brief Все события как непрерывный массив. Только для Layout::AoS.
     * @tparam E Тип события.
     */
    template<Event E>
    [[nodiscard]] std::span<const E> events() const noexcept;

    /**
     * @brief Колонка одного поля как непрерывный массив. Только для Layout::SoA.
     * @tparam E      Тип события.
     * @tparam Member Поле события, например `&VoxelChanged::block`.
     */
    template<Event E, auto Member>
    [[nodiscard]] std::span<const member_value_t<Member>> column() const noexcept;

    /**
     * @brief Значение поля события `index` при любой раскладке.
     * @pre `index < size()`.
     */
    template<Event E, auto Member>
    [[nodiscard]] const member_value_t<Member>& field(std::size_t index) const noexcept;

    /**
     * @brief Копия события `index` при любой раскладке (для SoA собирается из колонок).
     * @pre `index < size()`.
     */
    template<Event E>
    [[nodiscard]] E get(std::size_t index) const noexcept;

private:
    template<Event E>
    [[nodiscard]] bool matches() const noexcept {
        return m_schema->id == event_id_v<E> && m_schema->layout == event_layout_v<E> &&
               m_schema->size == sizeof(E);
    }

    template<typename Value>
    void store_field(ColumnBuffer& column, const Value& value) noexcept {
        if constexpr (std::is_array_v<Value>) {
            // Массивы нельзя копировать присваиванием; они trivially copyable, поэтому memcpy корректен.
            std::memcpy(column.at(m_size), std::addressof(value), sizeof(Value));
        } else {
            std::construct_at(reinterpret_cast<Value*>(column.data()) + m_size, value);
        }
    }

    void grow();
    void note_push() {
        if (m_tracing) m_causes.push_back(0);
    }

    const EventSchema* m_schema;
    std::vector<ColumnBuffer> m_columns;
    std::size_t m_size = 0;
    std::size_t m_capacity = 0;
    std::vector<std::uint64_t> m_causes; ///< Причины по событиям (только при трассировке).
    bool m_tracing = false;
};

// =============================================================================
// Реализация шаблонов
// =============================================================================

template<Event E>
void EventBuffer::push(const E& event) {
    assert(matches<E>() && "EventBuffer::push: event type does not match buffer schema");
    if (m_size == m_capacity) [[unlikely]] {
        grow();
    }
    // Типизированная запись: шаг известен при компиляции, объект создаётся прямо в колонке.
    // Побайтовый memcpy здесь заметно медленнее: он заставляет компилятор проводить событие
    // через стек и ломает store-forwarding (см. бенчмарки BM_Emit_*).
    if constexpr (event_layout_v<E> == Layout::AoS) {
        std::construct_at(reinterpret_cast<E*>(m_columns[0].data()) + m_size, event);
    } else {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (store_field<typename event_field_t<E, I>::value_type>(m_columns[I], event.*event_field_t<E, I>::member),
             ...);
        }(std::make_index_sequence<event_field_count_v<E>>{});
    }
    ++m_size;
    note_push();
}

template<Event E>
std::span<const E> EventBuffer::events() const noexcept {
    static_assert(event_layout_v<E> == Layout::AoS, "events() requires an AoS event; use column<E, &E::field>() for SoA");
    assert(matches<E>());
    return {reinterpret_cast<const E*>(m_columns[0].data()), m_size};
}

template<Event E, auto Member>
std::span<const member_value_t<Member>> EventBuffer::column() const noexcept {
    static_assert(is_field_of_v<E, Member>, "column(): field does not belong to event E");
    static_assert(event_layout_v<E> == Layout::SoA, "column() requires an SoA event; use events<E>() for AoS");
    assert(matches<E>());
    constexpr std::size_t index = field_index_v<E, Member>;
    return {reinterpret_cast<const member_value_t<Member>*>(m_columns[index].data()), m_size};
}

template<Event E, auto Member>
const member_value_t<Member>& EventBuffer::field(std::size_t index) const noexcept {
    static_assert(is_field_of_v<E, Member>, "field(): field does not belong to event E");
    assert(matches<E>() && index < m_size);
    if constexpr (event_layout_v<E> == Layout::AoS) {
        return reinterpret_cast<const E*>(m_columns[0].at(index))->*Member;
    } else {
        constexpr std::size_t column_index = field_index_v<E, Member>;
        return *reinterpret_cast<const member_value_t<Member>*>(m_columns[column_index].at(index));
    }
}

template<Event E>
E EventBuffer::get(std::size_t index) const noexcept {
    assert(matches<E>() && index < m_size);
    E result{};
    if constexpr (event_layout_v<E> == Layout::AoS) {
        std::memcpy(std::addressof(result), m_columns[0].at(index), sizeof(E));
    } else {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (std::memcpy(std::addressof(result.*event_field_t<E, I>::member),
                         m_columns[I].at(index),
                         sizeof(typename event_field_t<E, I>::value_type)),
             ...);
        }(std::make_index_sequence<event_field_count_v<E>>{});
    }
    return result;
}

} // namespace EventSystem
