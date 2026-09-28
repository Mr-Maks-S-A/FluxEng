#pragma once
/**
 * @file StreamChannel.hpp
 * @brief Канал с политикой Delivery::Stream (двойной буфер по тикам).
 */

#include <EventSystem/Channel/IChannel.hpp>

#include <cstddef>
#include <cstdint>

namespace EventSystem {

/**
 * @brief Канал «поток событий»: отправленное в тике N читается в тике N+1.
 *
 * Внутри два буфера:
 * - `pending` — сюда пишут системы в текущем тике;
 * - `ready` — отсюда читают системы в текущем тике (это события прошлого тика).
 *
 * advance() очищает `ready` и меняет буферы местами. Память переиспользуется,
 * поэтому в устойчивом режиме аллокаций нет.
 *
 * Свойства модели:
 * - порядок систем внутри тика не влияет на то, что они прочитают;
 * - каскад «событие → реакция → событие» растягивается по тикам и не может
 *   зациклить один тик;
 * - каждое событие видно ровно один тик: система, которая пропустила тик, пропустит и события.
 *
 * @note Не потокобезопасен: писать и читать из одного потока или синхронизировать снаружи.
 */
class StreamChannel final : public IChannel {
public:
    /**
     * @param schema Схема событий (канал хранит свою копию).
     * @param config Настройки; `config.delivery` должен быть Delivery::Stream.
     * @throws EventSystemError Если политика не Stream.
     */
    StreamChannel(EventSchema schema, const ChannelConfig& config);

    StreamChannel(const StreamChannel&) = delete;
    StreamChannel& operator=(const StreamChannel&) = delete;
    StreamChannel(StreamChannel&&) = delete;
    StreamChannel& operator=(StreamChannel&&) = delete;

    [[nodiscard]] const EventSchema& schema() const noexcept override { return m_schema; }
    [[nodiscard]] Delivery delivery() const noexcept override { return Delivery::Stream; }
    [[nodiscard]] const ChannelConfig& config() const noexcept override { return m_config; }
    void configure(const ChannelConfig& config) override;
    bool emit_raw(const std::byte* event) override;
    [[nodiscard]] const EventBuffer& readable() const noexcept override { return m_ready; }
    void advance() override;
    void clear() noexcept override;
    [[nodiscard]] ChannelStats stats() const noexcept override;

    // ------------------------------------------------------------ горячий путь (невиртуальный)

    /// @brief Буфер, в который пишут в текущем тике. Адрес не меняется за время жизни канала.
    [[nodiscard]] EventBuffer& pending() noexcept { return m_pending; }
    /// @brief Буфер, который читают в текущем тике. Адрес не меняется за время жизни канала.
    [[nodiscard]] const EventBuffer& ready() const noexcept { return m_ready; }
    /// @brief `true`, если бюджет тика ещё не исчерпан.
    [[nodiscard]] bool has_room() const noexcept { return m_pending.size() < m_limit; }
    /// @brief Учитывает отброшенное событие.
    void note_dropped() noexcept { ++m_total_dropped; }

private:
    void apply_config();

    // Порядок важен: буферы ссылаются на m_schema.
    EventSchema m_schema;
    ChannelConfig m_config;
    std::size_t m_limit = 0;
    EventBuffer m_pending;
    EventBuffer m_ready;

    std::size_t m_peak_per_tick = 0;
    std::uint64_t m_total_emitted = 0;
    std::uint64_t m_total_dropped = 0;
};

} // namespace EventSystem
