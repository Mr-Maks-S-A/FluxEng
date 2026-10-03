#pragma once
/**
 * @file Journal.hpp
 * @brief Журнал событий с избыточностью: дописывается только в конец, переживает повреждение блоков и обрыв записи.
 *
 * Два слоя, у каждого своя защита:
 *
 * ```
 *  события ──► записи  [sync | длина | номер | тик | тип | данные | CRC-32C]      логический поток байт
 *                 │                                                              (каждая запись проверяема сама)
 *                 ▼  режем на блоки по block_size и группируем по k штук
 *  полоса:  [данные 0] [данные 1] … [данные k−1] [чётность 0] … [чётность m−1]  физический слой
 *           у каждого блока: CRC-32C, номер полосы, номер блока               (Рид—Соломон: любые m блоков полосы)
 * ```
 *
 * - **Блок повреждён** (CRC не сошёлся) — он считается стёртым; полоса с ≤ m стёртыми блоками восстанавливается полностью.
 * - **Полоса потеряна** (стёрто > m блоков) — уцелевшие блоки данных всё равно читаются; записи, которые на них целиком
 *   лежат, возвращаются, остальные — в отчёте как *пропуски* по номерам (`Gap`). Чтение «перепрыгивает» через дыру
 *   по маркеру `sync` и контрольной сумме записи, поэтому одна дыра не теряет всё остальное.
 * - **Заголовок** хранится в двух копиях; достаточно одной.
 * - **Обрыв записи** (питание, сбой): полоса недописана — недостающие блоки считаются стёртыми и восстанавливаются, если можно.
 *
 * Возвращённая запись **всегда достоверна**: она прошла CRC. Ничего «почти правильного» читатель не отдаёт.
 *
 * @code
 * EventLog::MemoryStorage storage;                                    // или FileStorage::open(path, Mode::Create)
 * auto writer = EventLog::Writer::create(storage, {.data_blocks = 8, .parity_blocks = 2}).value();
 * writer.append(tick, MyEvent{...});                                  // типизированно; или append(tick, type, bytes)
 * writer.flush();                                                     // дописать полосу: устойчивая точка
 *
 * auto read = EventLog::read_all(storage).value();                    // записи и отчёт о восстановлении
 * for (const EventLog::Record& r : read.records)
 *     if (auto e = EventLog::decode<MyEvent>(r)) handle(*e);
 * @endcode
 */

#include <EventLog/ErasureCode.hpp>
#include <EventLog/Storage.hpp>

#include <concepts>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace EventLog {

/// @brief Форма избыточности журнала. Накладные расходы ≈ m/k (+ 12 байт на блок): при 8 + 2 — четверть.
struct Config {
    std::uint16_t data_blocks = 8;   ///< k: блоков данных в полосе.
    std::uint16_t parity_blocks = 2; ///< m: сколько блоков полосы можно потерять без потери данных.
    std::uint32_t block_size = 4096; ///< Байт данных в блоке (64 … 1 МиБ).
    [[nodiscard]] friend bool operator==(const Config&, const Config&) = default;
};

/// @brief Раскладка файла журнала (для инструментов, тестов и имитации повреждений): смещение блока `block` полосы `stripe` в байтах.
/// Блок на проводе занимает `12 + block_size` байт; перед полосами — две копии заголовка по 32 байта.
[[nodiscard]] constexpr std::uint64_t block_offset(const Config& config, std::uint64_t stripe, int block) noexcept {
    const std::uint64_t wire = 12 + config.block_size;
    return 64 + stripe * static_cast<std::uint64_t>(config.data_blocks + config.parity_blocks) * wire + static_cast<std::uint64_t>(block) * wire;
}
/// @brief Размер одного блока на проводе (данные + 12 байт служебных).
[[nodiscard]] constexpr std::uint64_t block_wire_size(const Config& config) noexcept { return 12 + config.block_size; }

/// @brief Идентификатор типа события: FNV-1a 32 бита от имени (`E::event_name`) — одинаков на любой машине.
[[nodiscard]] constexpr std::uint32_t type_id(std::string_view name) noexcept {
    std::uint32_t h = 2166136261u;
    for (const char c : name) h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
    return h;
}

/// @brief Событие, которое можно писать в журнал: тривиально копируемая структура с именем (как события EventSystem).
template<typename E>
concept LoggableEvent = std::is_trivially_copyable_v<E> && requires {
    { E::event_name } -> std::convertible_to<std::string_view>;
};

/// @brief Прочитанная запись.
struct Record {
    std::uint64_t sequence = 0;       ///< Сквозной номер записи (без дыр, пока ничего не потеряно).
    std::uint32_t tick = 0;           ///< Тик симуляции, в котором произошло событие.
    std::uint32_t type = 0;           ///< `type_id` имени события.
    std::vector<std::byte> payload;
    [[nodiscard]] friend bool operator==(const Record&, const Record&) = default;
};

/// @brief Распаковывает типизированное событие; `nullopt`, если тип или размер не совпали (событие другого типа или иной версии).
template<LoggableEvent E>
[[nodiscard]] std::optional<E> decode(const Record& record) {
    if (record.type != type_id(E::event_name) || record.payload.size() != sizeof(E)) return std::nullopt;
    E event;
    std::memcpy(&event, record.payload.data(), sizeof(E));
    return event;
}

/// @brief Диапазон потерянных номеров записей (включительно): записи существовали, но восстановить их не удалось.
struct Gap {
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    [[nodiscard]] friend bool operator==(const Gap&, const Gap&) = default;
};

/// @brief Что нашли и что исправили при чтении.
struct RecoveryReport {
    std::uint64_t stripes = 0;          ///< Полос в журнале.
    std::uint64_t blocks_total = 0;
    std::uint64_t blocks_corrupt = 0;   ///< Блоков, не прошедших проверку (CRC, номер, обрыв).
    std::uint64_t blocks_repaired = 0;  ///< Из них восстановлено по чётности.
    std::uint64_t blocks_lost = 0;      ///< Из них не восстановлено (в полосе стёрто больше m).
    std::uint64_t stripes_damaged = 0;  ///< Полос с хотя бы одним плохим блоком.
    std::uint64_t stripes_lost = 0;     ///< Полос, которые не восстановились.
    bool header_repaired = false;       ///< Одна из двух копий заголовка была повреждена.
    std::uint64_t records = 0;          ///< Записей прочитано.
    std::uint64_t records_lost = 0;     ///< Сколько номеров записей пропало (сумма `gaps`).
    std::uint64_t bytes_skipped = 0;    ///< Байт, пропущенных при поиске следующей записи (мусор после дыры).
    std::vector<Gap> gaps;              ///< Пропуски внутри известного диапазона. Потеря «хвоста» журнала отсюда не видна.
    /// @brief Журнал цел: ничего не повреждено и не потеряно.
    [[nodiscard]] bool clean() const noexcept { return blocks_corrupt == 0 && !header_repaired && gaps.empty() && bytes_skipped == 0; }
};

struct ReadResult {
    std::vector<Record> records; ///< В порядке номеров; каждая прошла CRC.
    RecoveryReport report;
};

/**
 * @brief Читает журнал, восстанавливая повреждённое по чётности.
 * @param repair `true` — исправленные блоки и заголовок переписываются в хранилище (журнал снова целый).
 * @return Ошибка только если разрушены обе копии заголовка (форму журнала не узнать); всё остальное — в отчёте.
 */
[[nodiscard]] std::expected<ReadResult, std::string> read_all(Storage& storage, bool repair = false);
/// @brief Только проверка: отчёт без записей и без изменений хранилища.
[[nodiscard]] std::expected<RecoveryReport, std::string> verify(Storage& storage);
/// @brief Проверка с исправлением на месте (`read_all(storage, true)` без возврата записей).
[[nodiscard]] std::expected<RecoveryReport, std::string> repair(Storage& storage);

class Writer {
public:
    /// @brief Новый журнал: хранилище очищается. Ошибка — неверная конфигурация или не пишется.
    [[nodiscard]] static std::expected<Writer, std::string> create(Storage& storage, const Config& config = {});
    /// @brief Продолжить существующий журнал: проверяет и чинит его, продолжает нумерацию и дописывает с новой полосы.
    [[nodiscard]] static std::expected<Writer, std::string> resume(Storage& storage);

    Writer(Writer&&) noexcept = default;
    Writer& operator=(Writer&&) noexcept = default;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer(); ///< Сбрасывает недописанную полосу (как `flush`).

    /// @brief Добавляет запись; возвращает её номер. Полоса пишется, когда накопится `k` блоков.
    std::uint64_t append(std::uint32_t tick, std::uint32_t type, std::span<const std::byte> payload);
    template<LoggableEvent E>
    std::uint64_t append(std::uint32_t tick, const E& event) {
        return append(tick, type_id(E::event_name), std::as_bytes(std::span<const E>(&event, 1)));
    }

    /**
     * @brief Дописывает текущую (неполную) полосу нулями и сбрасывает на носитель: устойчивая точка.
     * Частый flush расходует место (полоса целиком ради пары записей) — вызывать по расписанию, а не после каждой записи.
     */
    bool flush();

    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return m_sequence; }
    [[nodiscard]] std::uint64_t stripes_written() const noexcept { return m_stripe; }
    [[nodiscard]] const Config& config() const noexcept { return m_config; }

private:
    Writer(Storage& storage, const Config& config, std::uint64_t stripe, std::uint64_t sequence);
    bool emit_stripe();

    Storage* m_storage;
    Config m_config;
    ErasureCode m_code;
    std::uint64_t m_stripe;
    std::uint64_t m_sequence;
    std::vector<std::byte> m_pending; ///< Логический поток, ещё не ушедший в полосу.
};

} // namespace EventLog
