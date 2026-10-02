#pragma once
/**
 * @file Channel.hpp
 * @brief Канал событий: буферы, политика доставки, часы, дорожки потоков, причины.
 */

#include <EventSystem/Channel/IChannel.hpp>
#include <EventSystem/Core/Event.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace EventSystem {

/**
 * @brief Единственная реализация IChannel — для всех политик доставки и обоих доменов времени.
 *
 * Внутри:
 * ```
 *  писатели (главный поток) ──▶ pending ──┐
 *  дорожки потоков 0..N   ──▶ lanes[i] ──┤ advance(): дорожки → pending (по порядку, бюджет)
 *  emit_after (Scheduled) ──▶ future  ──┤            политика: pending/future → ready
 *                                         ▼
 *                                       ready ──▶ читатели (видно весь следующий момент)
 * ```
 *
 * **Горячий путь одинаков у всех политик**: писатель кладёт событие в `pending` (или в свою дорожку),
 * читатель смотрит `ready`. Политика (Stream / Coalesced / Scheduled) работает только в advance() —
 * поэтому новая политика не меняет ни EventWriter, ни EventReader, ни код игр.
 *
 * **Дорожки потоков** (open_lanes): у каждого куска параллельной работы — свой EventBuffer
 * той же схемы и раскладки (SoA остаётся SoA). Запись в дорожку — без мьютексов и атомиков: дорожку
 * пишет только её кусок. В advance() дорожки дописываются в `pending` по порядку номеров (memcpy на колонку),
 * поэтому результат не зависит от числа потоков. Бюджет канала применяется при слиянии.
 *
 * @note Регистрация, open_lanes() и advance() — из одного (главного) потока; запись в разные дорожки — из любых.
 */
class Channel final : public IChannel {
public:
    /**
     * @param schema Схема событий (канал хранит свою копию).
     * @param config Настройки (политика и домен задаются здесь раз и навсегда).
     * @throws EventSystemError Неверный ключ слияния для Coalesced.
     */
    Channel(EventSchema schema, const ChannelConfig& config);
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    [[nodiscard]] const EventSchema& schema() const noexcept override { return m_schema; }
    [[nodiscard]] Delivery delivery() const noexcept override { return m_config.delivery; }
    [[nodiscard]] Domain domain() const noexcept override { return m_config.domain; }
    [[nodiscard]] const ChannelConfig& config() const noexcept override { return m_config; }
    void configure(const ChannelConfig& config) override;
    bool emit_raw(const std::byte* event) override;
    [[nodiscard]] const EventBuffer& readable() const noexcept override { return m_ready; }
    void advance() override;
    void clear() noexcept override;
    [[nodiscard]] ChannelStats stats() const noexcept override;

    // ------------------------------------------------------------ горячий путь (невиртуальный)

    /// @brief Буфер, в который пишут в текущем моменте. Адрес не меняется за время жизни канала.
    [[nodiscard]] EventBuffer& pending() noexcept { return m_pending; }
    /// @brief Буфер, который читают в текущем моменте. Адрес не меняется за время жизни канала.
    [[nodiscard]] const EventBuffer& ready() const noexcept { return m_ready; }
    /// @brief `true`, если бюджет момента ещё не исчерпан (без учёта дорожек — они проверяются при слиянии).
    [[nodiscard]] bool has_room() const noexcept { return m_pending.size() < m_limit; }
    /// @brief Учитывает отброшенное событие.
    void note_dropped() noexcept { ++m_total_dropped; }

    /// @brief Scheduled: событие станет видно через `delay` моментов (1 — как обычный emit).
    template<Event E>
    void schedule(const E& event, Tick delay, EventRef cause) {
        assert(m_config.delivery == Delivery::Scheduled && "schedule(): channel is not Scheduled");
        m_future.push(event);
        m_future.set_last_cause(cause);
        m_due.push_back(m_time + (delay == 0 ? 1 : delay));
    }

    // ------------------------------------------------------------ дорожки потоков

    /**
     * @brief Выделяет `count` дорожек на текущий момент; возвращает номер первой.
     *
     * Вызывать из главного потока до параллельной работы. Несколько систем могут открыть дорожки
     * в одном моменте — они сольются в порядке открытия, затем по номерам. Память дорожек переиспользуется.
     */
    std::uint32_t open_lanes(std::uint32_t count);
    /// @brief Дорожка `index` (из open_lanes). Пишет в неё только один поток.
    [[nodiscard]] EventBuffer& lane(std::uint32_t index) noexcept {
        assert(index < m_lanes_open && "lane(): lane is not open in this moment");
        return m_lanes[index].buffer;
    }

    // ------------------------------------------------------------ время и ссылки

    /// @brief Номер канала в шине (задаёт EventBus при регистрации).
    [[nodiscard]] std::uint32_t index() const noexcept { return m_index; }
    void set_index(std::uint32_t index) noexcept { m_index = index; }
    /// @brief Сколько раз канал сменил момент: «время», к которому относятся видимые события.
    [[nodiscard]] Tick time() const noexcept { return m_time; }
    /// @brief Ссылка на видимое событие `index` (для причин и отладки).
    [[nodiscard]] EventRef ref(std::size_t index) const noexcept {
        return EventRef::make(m_time, m_index, static_cast<std::uint32_t>(index));
    }

private:
    struct alignas(64) Lane { // своя кэш-линия: счётчики соседних дорожек не мешают друг другу
        explicit Lane(const EventSchema& schema) : buffer(schema) {}
        EventBuffer buffer;
    };

    void apply_config();
    void merge_lanes();
    void deliver_coalesced();
    void deliver_scheduled();
    [[nodiscard]] std::uint64_t key_of(const EventBuffer& buffer, std::size_t index) const noexcept;

    // Порядок важен: буферы ссылаются на m_schema.
    EventSchema m_schema;
    ChannelConfig m_config;
    std::size_t m_limit = 0;
    EventBuffer m_pending;
    EventBuffer m_ready;
    EventBuffer m_future;   ///< Scheduled: ждущие события.
    EventBuffer m_kept;     ///< Scheduled: временный буфер при отборе (память переиспользуется).
    std::vector<Tick> m_due;
    std::vector<Tick> m_kept_due;
    std::vector<Lane> m_lanes;
    std::uint32_t m_lanes_open = 0;
    std::unordered_map<std::uint64_t, std::size_t> m_keys; ///< Coalesced: ключ → место в ready.
    std::uint32_t m_index = 0;
    Tick m_time = 0;
    std::size_t m_peak_per_tick = 0;
    std::uint64_t m_total_emitted = 0;
    std::uint64_t m_total_dropped = 0;
    std::uint64_t m_total_coalesced = 0;
};

} // namespace EventSystem
