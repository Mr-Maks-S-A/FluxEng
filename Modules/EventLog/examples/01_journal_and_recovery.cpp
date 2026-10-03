/**
 * @example 01_journal_and_recovery.cpp
 * Журнал событий с избыточностью: запись, повреждение «носителя», чтение с восстановлением и отчёт.
 *
 * Схема защиты: записи с CRC-32C → блоки → полосы (k блоков данных + m блоков чётности, код Рида—Соломона).
 * Любые m испорченных блоков полосы восстанавливаются; если потеряно больше — уцелевшие записи всё равно читаются,
 * а пропуски названы по номерам. Читатель никогда не отдаёт недостоверную запись.
 */

#include <EventLog/Journal.hpp>

#include <cstdio>
#include <filesystem>

using namespace EventLog;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("ОШИБКА: %s (строка %d)\n", #cond, __LINE__);         \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/// Событие игры: тривиально копируемая структура с именем (как события EventSystem).
struct DamageEvent {
    std::int32_t target = 0;
    std::int32_t amount = 0;
    static constexpr std::string_view event_name = "game.damage";
};

static void print_report(const char* title, const RecoveryReport& r) {
    std::printf("   %s: полос %llu, блоков испорчено %llu (восстановлено %llu, потеряно %llu), записей прочитано %llu, потеряно %llu%s\n", title,
                static_cast<unsigned long long>(r.stripes), static_cast<unsigned long long>(r.blocks_corrupt), static_cast<unsigned long long>(r.blocks_repaired),
                static_cast<unsigned long long>(r.blocks_lost), static_cast<unsigned long long>(r.records), static_cast<unsigned long long>(r.records_lost), r.clean() ? " — журнал цел" : "");
}

int main() {
    // 1. Журнал в памяти (для файла — FileStorage::open(путь, Mode::Create)). Полоса: 4 блока данных + 2 блока чётности по 256 байт:
    //    выдерживает потерю любых двух блоков из шести; накладные расходы — треть. На практике — 8 + 2 по 4 КиБ (четверть).
    MemoryStorage storage;
    const Config config{.data_blocks = 4, .parity_blocks = 2, .block_size = 256};
    {
        auto writer = Writer::create(storage, config).value();
        for (int tick = 0; tick < 400; ++tick) writer.append(static_cast<std::uint32_t>(tick), DamageEvent{tick % 7, tick * 3});
        EXPECT(writer.flush()); // устойчивая точка: недописанная полоса добивается и сбрасывается на носитель
        std::printf("1. записано %llu событий в %llu полос(ы), размер журнала %zu байт\n", static_cast<unsigned long long>(writer.next_sequence()),
                    static_cast<unsigned long long>(writer.stripes_written()), storage.bytes().size());
    }
    const auto clean = read_all(storage).value();
    print_report("чтение целого журнала", clean.report);
    EXPECT(clean.report.clean() && clean.records.size() == 400);

    // 2. «Сбой носителя»: портим по одному-два блока в нескольких полосах (в пределах m = 2) — как битая секция диска.
    const std::size_t wire = 12 + config.block_size, stripe = 6 * wire, base = 64; // формат: см. Journal.hpp
    for (const auto [s, b] : {std::pair{0u, 1u}, {2u, 0u}, {2u, 3u}, {4u, 5u}}) storage.bytes()[base + s * stripe + b * wire + 40] ^= std::byte{0xFF};
    const auto damaged = read_all(storage).value();
    print_report("чтение после повреждений", damaged.report);
    EXPECT(damaged.report.blocks_corrupt == 4 && damaged.report.blocks_repaired == 4);
    EXPECT(damaged.records.size() == 400); // все записи на месте: потери отсутствуют
    int checked = 0;
    for (const Record& r : damaged.records) {
        const auto e = decode<DamageEvent>(r); // типизированное чтение: тип и размер проверяются
        EXPECT(e.has_value() && e->amount == static_cast<std::int32_t>(r.tick) * 3);
        ++checked;
    }
    std::printf("2. после 4 испорченных блоков все %d событий прочитаны и совпали\n", checked);

    // 3. repair() переписывает исправленные блоки на носитель: дальше журнал снова целый, запас прочности восстановлен.
    EXPECT(!verify(storage)->clean());
    const auto fixed = repair(storage).value();
    print_report("repair", fixed);
    EXPECT(verify(storage)->clean());

    // 4. Катастрофа: в одной полосе испорчено три блока (больше m = 2). Полоса потеряна, остальное читается.
    for (const std::size_t b : {0u, 1u, 2u}) storage.bytes()[base + 3 * stripe + b * wire + 40] ^= std::byte{0xFF};
    const auto lost = read_all(storage).value();
    print_report("потеряна полоса №3", lost.report);
    std::printf("4. вернулось %zu событий из 400; пропуски:", lost.records.size());
    for (const Gap& g : lost.report.gaps) std::printf(" [%llu…%llu]", static_cast<unsigned long long>(g.first_sequence), static_cast<unsigned long long>(g.last_sequence));
    std::printf("\n");
    EXPECT(lost.report.stripes_lost == 1 && lost.records.size() < 400 && lost.records.size() > 300 && !lost.report.gaps.empty());
    for (const Record& r : lost.records) EXPECT(decode<DamageEvent>(r)->amount == static_cast<std::int32_t>(r.tick) * 3); // уцелевшие — достоверны

    // 5. Журнал на диске: тот же интерфейс, другое хранилище. resume() чинит и продолжает с нужного номера.
    const auto path = std::filesystem::temp_directory_path() / "example_eventlog.journal";
    {
        auto file = FileStorage::open(path, FileStorage::Mode::Create);
        EXPECT(file != nullptr);
        auto writer = Writer::create(*file, config).value();
        for (int i = 0; i < 100; ++i) writer.append(static_cast<std::uint32_t>(i), DamageEvent{i, i});
    }
    {
        auto file = FileStorage::open(path, FileStorage::Mode::Existing);
        auto writer = Writer::resume(*file).value(); // после «перезапуска игры»
        std::printf("5. журнал на диске продолжен с номера %llu\n", static_cast<unsigned long long>(writer.next_sequence()));
        EXPECT(writer.next_sequence() == 100);
        writer.append(100, DamageEvent{100, 100});
    }
    auto file = FileStorage::open(path, FileStorage::Mode::Existing);
    EXPECT(read_all(*file)->records.size() == 101);
    std::filesystem::remove(path);
    std::printf("OK\n");
    return 0;
}
