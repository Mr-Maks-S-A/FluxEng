#pragma once
/**
 * @file IChannel.hpp
 * @brief Интерфейс канала событий, его конфигурация и статистика.
 */

#include <EventSystem/Core/Ids.hpp>
#include <EventSystem/Core/Schema.hpp>
#include <EventSystem/Storage/EventBuffer.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace EventSystem {

/**
 * @brief Политика доставки («пакетирования») событий канала.
 *
 * Определяет, когда события становятся видимы читателям и сколько живут.
 * Сейчас реализована только Delivery::Stream; перечисление — точка расширения
 * для будущих политик (Bulk, Coalesced, Scheduled, Log), которые добавляются
 * новыми реализациями IChannel без изменения шины и существующих событий.
 */
enum class Delivery : std::uint8_t {
    /**
     * Двойной буфер по тикам: событие, отправленное в тике N,
     * видно всем читателям ровно в течение тика N+1.
     */
    Stream,
};

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
};

/**
 * @brief Снимок статистики канала — для профилирования, отладочного UI и тестов.
 */
struct ChannelStats {
    std::size_t pending = 0;          ///< Событий отправлено в текущем тике (станут видны в следующем).
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
    /// @brief Текущие настройки.
    [[nodiscard]] virtual const ChannelConfig& config() const noexcept = 0;

    /**
     * @brief Меняет настройки (резерв, бюджет).
     * @throws EventSystemError Если `config.delivery` отличается от политики канала.
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

    /// @brief Переход к следующему тику (вызывается шиной).
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
