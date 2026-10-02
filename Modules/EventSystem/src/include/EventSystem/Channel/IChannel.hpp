#pragma once
/**
 * @file IChannel.hpp
 * @brief Интерфейс канала событий, его конфигурация и статистика.
 */

#include <EventSystem/Core/Event.hpp>
#include <EventSystem/Core/Ids.hpp>
#include <EventSystem/Core/Schema.hpp>
#include <EventSystem/Storage/EventBuffer.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace EventSystem {

/**
 * @brief Политика доставки событий канала: что из отправленного и когда видят читатели.
 *
 * Политика влияет только на смену тика (холодный путь). Горячий путь — запись в буфер текущего
 * тика и чтение видимых событий — у всех политик один и тот же, поэтому EventWriter / EventReader
 * работают с любым каналом без виртуальных вызовов.
 */
enum class Delivery : std::uint8_t {
    /** Двойной буфер: отправленное в тике N видно ровно в тике N+1. */
    Stream,
    /**
     * Слияние: из событий тика с одинаковым ключом (поле ChannelConfig::coalesce_field) остаётся последнее,
     * на месте первого. «Клетка изменилась» 10 раз за тик → одно событие. Замена ручных dirty-флагов.
     */
    Coalesced,
    /**
     * Отложенные события: EventWriter::emit_after(e, N) станет видно через N тиков (emit — через 1).
     * Отрастание деревьев, перезарядка, «молния через секунду» — без ручных очередей в игре.
     */
    Scheduled,
};

/**
 * @brief Часы канала: какой вызов шины делает отправленное видимым.
 *
 * Tick — EventBus::advance_tick() (симуляция; на паузе стоит). Frame — EventBus::advance_frame()
 * (каждый кадр, и на паузе тоже: интерфейс, ввод для UI, камера).
 */
enum class Domain : std::uint8_t { Tick, Frame };

/// @brief Имя домена для вывода.
[[nodiscard]] std::string_view to_string(Domain domain) noexcept;

/// @brief Имя политики для вывода.
[[nodiscard]] std::string_view to_string(Delivery delivery) noexcept;

/**
 * @brief Настройки канала.
 */
struct ChannelConfig {
    /// @brief Сколько событий зарезервировать заранее (в каждом из буферов канала).
    std::size_t reserve = 0;

    /**
     * @brief Бюджет: максимум событий за один тик; 0 — без ограничения.
     *
     * Защищает от «взрыва» событий, например от зацикленного заклинания,
     * собранного игроком. Лишние события отбрасываются, `emit` возвращает `false`,
     * счётчик ChannelStats::total_dropped растёт.
     */
    std::size_t max_events_per_tick = 0;

    /// @brief Политика доставки. Не меняется после создания канала.
    Delivery delivery = Delivery::Stream;
    /// @brief Часы канала. Не меняются после создания канала.
    Domain domain = Domain::Tick;
    /// @brief Coalesced: индекс поля-ключа в схеме (см. coalesce_key<&E::field>()). Поле не длиннее 8 байт.
    std::size_t coalesce_field = 0;
    /// @brief Хранить причину каждого события (дерево причин, EventBus::cause_chain()). Стоит 8 байт на событие.
    bool trace = false;
};

/// @brief Индекс поля `Member` события `E` для ChannelConfig::coalesce_field.
template<Event E, auto Member>
[[nodiscard]] constexpr std::size_t coalesce_key() noexcept {
    static_assert(is_field_of_v<E, Member>, "coalesce_key: field does not belong to event E");
    static_assert(sizeof(member_value_t<Member>) <= 8, "coalesce_key: key field must be at most 8 bytes");
    return field_index_v<E, Member>;
}

/**
 * @brief Снимок статистики канала — для профилирования, отладочного UI и тестов.
 */
struct ChannelStats {
    std::size_t pending = 0;          ///< Событий отправлено в текущем тике (станут видны в следующем), без дорожек.
    std::size_t scheduled = 0;        ///< Scheduled: ждут своего тика.
    std::size_t lanes = 0;            ///< Дорожек потоков выделено (память переиспользуется).
    std::uint64_t total_coalesced = 0; ///< Coalesced: событий поглощено слиянием.
    std::size_t readable = 0;         ///< Событий доступно читателям в текущем тике.
    std::size_t peak_per_tick = 0;    ///< Максимум событий за один тик за всё время.
    std::uint64_t total_emitted = 0;  ///< Всего принято событий (учитываются при смене тика).
    std::uint64_t total_dropped = 0;  ///< Всего отброшено событий из-за бюджета.
    std::size_t allocated_bytes = 0;  ///< Выделено памяти всеми буферами канала, байт.
};

/**
 * @brief Канал: хранилище событий одного типа плюс политика доставки.
 *
 * Виртуальный интерфейс используется только на холодных путях
 * (регистрация, смена тика, инструменты, скрипты). Горячий путь C++-систем
 * идёт через EventWriter / EventReader, которые работают с конкретной
 * реализацией канала напрямую, без виртуальных вызовов.
 */
class IChannel {
public:
    virtual ~IChannel() = default;

    /// @brief Схема событий канала.
    [[nodiscard]] virtual const EventSchema& schema() const noexcept = 0;
    /// @brief Политика доставки.
    [[nodiscard]] virtual Delivery delivery() const noexcept = 0;
    /// @brief Часы канала.
    [[nodiscard]] virtual Domain domain() const noexcept = 0;
    /// @brief Текущие настройки.
    [[nodiscard]] virtual const ChannelConfig& config() const noexcept = 0;

    /**
     * @brief Меняет настройки (резерв, бюджет).
     * @throws EventSystemError Если `config.delivery` или `config.domain` отличаются от заданных при создании.
     */
    virtual void configure(const ChannelConfig& config) = 0;

    /**
     * @brief Отправляет событие из байтового AoS-представления (путь для скриптов).
     * @param event `schema().size` байт.
     * @return `false`, если событие отброшено из-за бюджета.
     */
    virtual bool emit_raw(const std::byte* event) = 0;

    /// @brief События, видимые читателям в текущем тике.
    [[nodiscard]] virtual const EventBuffer& readable() const noexcept = 0;

    /// @brief Переход к следующему моменту своего домена (вызывается шиной): дорожки сливаются, политика решает, что видно.
    virtual void advance() = 0;

    /// @brief Удаляет все события (и отправленные, и видимые). Статистика не сбрасывается.
    virtual void clear() noexcept = 0;

    /// @brief Снимок статистики.
    [[nodiscard]] virtual ChannelStats stats() const noexcept = 0;

    /// @brief Идентификатор события канала.
    [[nodiscard]] EventId id() const noexcept { return schema().id; }
    /// @brief Имя события канала.
    [[nodiscard]] std::string_view name() const noexcept { return schema().name; }
};

} // namespace EventSystem
