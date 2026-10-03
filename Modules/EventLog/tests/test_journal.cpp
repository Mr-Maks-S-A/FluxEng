#include <EventLog/Journal.hpp>

#include <Math/Rng.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

using namespace EventLog;

namespace {

struct Hit {
    std::int32_t x = 0, y = 0, damage = 0;
    static constexpr std::string_view event_name = "test.hit";
};
struct Heal {
    std::int32_t amount = 0;
    static constexpr std::string_view event_name = "test.heal";
};

const Config small{.data_blocks = 4, .parity_blocks = 2, .block_size = 128}; // маленькие полосы: много полос на мало записей
constexpr std::size_t wire = 12 + 128;                                       // блок на проводе: 12 байт служебных + данные
constexpr std::size_t base = 64;                                             // две копии заголовка по 32 байта
constexpr std::size_t stripe_size = 6 * wire;

std::size_t block_offset(std::size_t stripe, std::size_t block) { return base + stripe * stripe_size + block * wire; }

/// Журнал из `count` записей случайной длины; payload восстанавливается по номеру (для проверки достоверности).
std::vector<std::byte> payload_for(std::uint64_t sequence, std::size_t size_hint) {
    Math::Rng rng(sequence * 7919 + 13);
    std::vector<std::byte> p(size_hint % 150);
    for (std::byte& b : p) b = static_cast<std::byte>(rng.next_u32());
    return p;
}

void fill(MemoryStorage& storage, int count, const Config& config = small) {
    auto writer = Writer::create(storage, config).value();
    for (int i = 0; i < count; ++i) {
        const auto p = payload_for(static_cast<std::uint64_t>(i), static_cast<std::size_t>(i) * 37 + 5);
        writer.append(static_cast<std::uint32_t>(i / 3), static_cast<std::uint32_t>(i % 5), p);
    }
    REQUIRE(writer.flush());
}

void corrupt_block(MemoryStorage& storage, std::size_t stripe, std::size_t block, std::uint64_t seed = 1) {
    Math::Rng rng(seed + stripe * 31 + block);
    auto& bytes = storage.bytes();
    for (int flips = 0; flips < 3; ++flips) {
        const std::size_t at = block_offset(stripe, block) + rng.below(static_cast<std::uint32_t>(wire));
        bytes[at] ^= static_cast<std::byte>(1 + rng.below(255));
    }
}

bool all_intact(const ReadResult& r, int count) {
    if (r.records.size() != static_cast<std::size_t>(count)) return false;
    for (const Record& rec : r.records) {
        const auto i = rec.sequence;
        if (rec.payload != payload_for(i, i * 37 + 5) || rec.tick != i / 3 || rec.type != i % 5) return false;
    }
    return true;
}

} // namespace

TEST_CASE("запись и чтение: порядок, содержимое, чистый отчёт") {
    MemoryStorage storage;
    fill(storage, 200);
    const auto read = read_all(storage).value();
    CHECK(all_intact(read, 200));
    CHECK(read.report.clean());
    CHECK(read.report.records == 200);
    CHECK(read.report.stripes > 5);
    CHECK(read.report.blocks_total == read.report.stripes * 6);
}

TEST_CASE("пустой журнал, журнал из одной записи и записи нулевой длины") {
    MemoryStorage storage;
    { auto w = Writer::create(storage, small).value(); }
    CHECK(read_all(storage)->records.empty());
    CHECK(read_all(storage)->report.clean());

    {
        auto w = Writer::create(storage, small).value();
        w.append(7, 1, {});
        w.append(8, 2, {});
    } // деструктор сбрасывает полосу
    const auto r = read_all(storage).value();
    REQUIRE(r.records.size() == 2);
    CHECK(r.records[0].payload.empty());
    CHECK(r.records[1].tick == 8);
}

TEST_CASE("типизированные события: тип по имени, разные типы вперемешку, чужой тип не распаковывается") {
    MemoryStorage storage;
    {
        auto w = Writer::create(storage, small).value();
        for (int i = 0; i < 50; ++i) {
            w.append(static_cast<std::uint32_t>(i), Hit{i, -i, i * 2});
            w.append(static_cast<std::uint32_t>(i), Heal{i + 100});
        }
    }
    const auto read = read_all(storage).value();
    REQUIRE(read.records.size() == 100);
    int hits = 0, heals = 0;
    for (const Record& r : read.records) {
        if (const auto h = decode<Hit>(r)) {
            CHECK(h->damage == h->x * 2);
            ++hits;
        } else if (const auto e = decode<Heal>(r)) {
            CHECK(e->amount >= 100);
            ++heals;
        }
        CHECK_FALSE((decode<Hit>(r).has_value() && decode<Heal>(r).has_value()));
    }
    CHECK(hits == 50);
    CHECK(heals == 50);
    CHECK(type_id(Hit::event_name) != type_id(Heal::event_name));
    static_assert(type_id("a") == 0xE40C292Cu); // контрольный вектор FNV-1a 32: одинаков на любой машине и в любой сборке
    CHECK(type_id("test.hit") == type_id(Hit::event_name));
}

TEST_CASE("один испорченный блок в каждой полосе восстанавливается полностью") {
    MemoryStorage storage;
    fill(storage, 300);
    const std::uint64_t stripes = verify(storage)->stripes;
    for (std::size_t s = 0; s < stripes; ++s) corrupt_block(storage, s, s % 6);
    const auto read = read_all(storage).value();
    CHECK(all_intact(read, 300));
    CHECK(read.report.blocks_corrupt == stripes);
    CHECK(read.report.blocks_repaired == stripes);
    CHECK(read.report.blocks_lost == 0);
    CHECK(read.report.stripes_damaged == stripes);
    CHECK(read.report.gaps.empty());
    CHECK_FALSE(read.report.clean()); // повреждения были, хоть и исправлены
}

TEST_CASE("до m испорченных блоков в полосе (в том числе блоки чётности) — данные целы") {
    MemoryStorage storage;
    fill(storage, 300);
    corrupt_block(storage, 1, 0);
    corrupt_block(storage, 1, 3);  // два блока данных
    corrupt_block(storage, 3, 2);
    corrupt_block(storage, 3, 5);  // данные + чётность
    corrupt_block(storage, 5, 4);
    corrupt_block(storage, 5, 5);  // обе чётности: данные не тронуты
    const auto read = read_all(storage).value();
    CHECK(all_intact(read, 300));
    CHECK(read.report.blocks_repaired == 6);
    CHECK(read.report.stripes_lost == 0);
}

TEST_CASE("полоса потеряна (стёрто больше m): уцелевшие записи читаются, пропуски названы точно") {
    MemoryStorage storage;
    fill(storage, 300);
    for (const std::size_t b : {0u, 1u, 2u}) corrupt_block(storage, 4, b); // три блока данных при m = 2
    const auto read = read_all(storage).value();
    CHECK(read.report.stripes_lost == 1);
    CHECK(read.report.blocks_lost == 3);
    CHECK(read.records.size() < 300);
    CHECK(read.records.size() > 250); // потеряна малая часть
    // Каждая вернувшаяся запись достоверна и на своём месте.
    std::set<std::uint64_t> got;
    for (const Record& r : read.records) {
        CHECK(r.payload == payload_for(r.sequence, r.sequence * 37 + 5));
        got.insert(r.sequence);
    }
    // Пропуски — ровно те номера, которых нет среди прочитанных (внутри известного диапазона).
    std::set<std::uint64_t> in_gaps;
    for (const Gap& g : read.report.gaps)
        for (std::uint64_t s = g.first_sequence; s <= g.last_sequence; ++s) in_gaps.insert(s);
    std::set<std::uint64_t> missing;
    for (std::uint64_t s = 0; s <= *got.rbegin(); ++s)
        if (!got.contains(s)) missing.insert(s);
    CHECK(in_gaps == missing);
    CHECK(read.report.records_lost == missing.size());
    CHECK(read.report.gaps.size() >= 1);
}

TEST_CASE("разрушена одна из двух копий заголовка — журнал читается и чинится; обе — ошибка") {
    MemoryStorage storage;
    fill(storage, 50);
    storage.bytes()[3] ^= std::byte{0xFF}; // первая копия
    auto read = read_all(storage).value();
    CHECK(all_intact(read, 50));
    CHECK(read.report.header_repaired);
    CHECK(repair(storage)->header_repaired);
    CHECK(verify(storage)->clean()); // копия восстановлена
    storage.bytes()[3] ^= std::byte{0xFF};
    storage.bytes()[40] ^= std::byte{0xFF}; // обе
    CHECK_FALSE(read_all(storage).has_value());
    MemoryStorage junk;
    junk.bytes().assign(100, std::byte{0x42});
    CHECK_FALSE(read_all(junk).has_value());
}

TEST_CASE("repair переписывает исправленные блоки: потом журнал чистый, и даже новое повреждение не накапливается") {
    MemoryStorage storage;
    fill(storage, 200);
    corrupt_block(storage, 2, 1);
    corrupt_block(storage, 2, 2);
    CHECK_FALSE(verify(storage)->clean());
    const auto fixed = repair(storage).value();
    CHECK(fixed.blocks_repaired == 2);
    CHECK(verify(storage)->clean());
    // Второе повреждение того же места теперь снова в пределах m, а не «два + два».
    corrupt_block(storage, 2, 0);
    corrupt_block(storage, 2, 4);
    CHECK(all_intact(read_all(storage).value(), 200));
}

TEST_CASE("обрыв записи: недописанная последняя полоса — потеря чётности не страшна, потеря данных видна, всё до обрыва цело") {
    MemoryStorage whole;
    fill(whole, 300);
    const std::uint64_t stripes = verify(whole)->stripes;

    // Оборвало внутри чётности: все блоки данных на месте, не хватает последнего блока чётности (потеряно 1 ≤ m).
    MemoryStorage parity_torn = whole;
    parity_torn.bytes().resize(block_offset(stripes - 1, 5) + wire / 2);
    const auto r1 = read_all(parity_torn, /*repair=*/true).value();
    CHECK(all_intact(r1, 300));
    CHECK(r1.report.blocks_repaired == 1);
    CHECK(verify(parity_torn)->clean()); // исправление дописало недостающее: журнал снова целый
    CHECK(parity_torn.bytes().size() == whole.bytes().size());

    // Оборвало раньше: уцелело 3 блока из 6 (< k = 4) — полоса потеряна, но всё до неё читается.
    MemoryStorage data_torn = whole;
    data_torn.bytes().resize(block_offset(stripes - 1, 3));
    const auto r2 = read_all(data_torn).value();
    CHECK(r2.report.stripes_lost == 1);
    CHECK(r2.records.size() > 200); // потеряны записи только последней полосы
    for (const Record& r : r2.records) CHECK(r.payload == payload_for(r.sequence, r.sequence * 37 + 5));
}

TEST_CASE("resume: журнал продолжается с нужного номера, в том числе после повреждения") {
    MemoryStorage storage;
    fill(storage, 120);
    corrupt_block(storage, 1, 2); // повреждение до дозаписи
    {
        auto writer = Writer::resume(storage).value();
        CHECK(writer.next_sequence() == 120);
        for (int i = 120; i < 180; ++i) writer.append(static_cast<std::uint32_t>(i / 3), static_cast<std::uint32_t>(i % 5), payload_for(static_cast<std::uint64_t>(i), static_cast<std::size_t>(i) * 37 + 5));
    }
    const auto read = read_all(storage).value();
    CHECK(all_intact(read, 180)); // resume починил блок и дописал; нумерация сквозная
    CHECK(read.report.clean());
}

TEST_CASE("flush на границе: устойчивая точка, записи после неё продолжают нумерацию") {
    MemoryStorage storage;
    auto writer = Writer::create(storage, small).value();
    writer.append(1, 1, payload_for(0, 5));
    CHECK(writer.flush());
    CHECK(writer.stripes_written() == 1);
    CHECK(read_all(storage)->records.size() == 1); // запись уже в журнале, хотя полоса не заполнена
    writer.append(2, 2, payload_for(1, 42));
    CHECK(writer.flush());
    CHECK(read_all(storage)->records.size() == 2);
    CHECK(writer.flush()); // повторный flush без данных ничего не пишет
    CHECK(writer.stripes_written() == 2);
}

TEST_CASE("большие записи шире блока и полосы") {
    MemoryStorage storage;
    {
        auto w = Writer::create(storage, small).value();
        std::vector<std::byte> big(5000);
        for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>(i * 7);
        w.append(1, 9, big);
        w.append(2, 9, big);
    }
    corrupt_block(storage, 1, 1);
    const auto read = read_all(storage).value();
    REQUIRE(read.records.size() == 2);
    CHECK(read.records[1].payload.size() == 5000);
    CHECK(read.records[0].payload[123] == static_cast<std::byte>((123 * 7) & 0xFF));
}

TEST_CASE("конфигурация проверяется") {
    MemoryStorage storage;
    CHECK_FALSE(Writer::create(storage, {.data_blocks = 0, .parity_blocks = 1, .block_size = 128}).has_value());
    CHECK_FALSE(Writer::create(storage, {.data_blocks = 4, .parity_blocks = 0, .block_size = 128}).has_value());
    CHECK_FALSE(Writer::create(storage, {.data_blocks = 4, .parity_blocks = 2, .block_size = 8}).has_value());
    CHECK_FALSE(Writer::create(storage, {.data_blocks = 200, .parity_blocks = 100, .block_size = 128}).has_value());
    CHECK(Writer::create(storage, {.data_blocks = 1, .parity_blocks = 1, .block_size = 64}).has_value());
}

TEST_CASE("файловое хранилище: запись, чтение, повреждение на диске, восстановление") {
    const auto path = std::filesystem::temp_directory_path() / "flux_eventlog_test.journal";
    {
        auto storage = FileStorage::open(path, FileStorage::Mode::Create);
        REQUIRE(storage);
        auto writer = Writer::create(*storage, small).value();
        for (int i = 0; i < 100; ++i) writer.append(static_cast<std::uint32_t>(i / 3), static_cast<std::uint32_t>(i % 5), payload_for(static_cast<std::uint64_t>(i), static_cast<std::size_t>(i) * 37 + 5));
        REQUIRE(writer.flush());
    }
    {
        auto storage = FileStorage::open(path, FileStorage::Mode::Existing);
        REQUIRE(storage);
        std::byte junk[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
        REQUIRE(storage->write(block_offset(1, 2) + 20, junk)); // порча на диске
    }
    auto storage = FileStorage::open(path, FileStorage::Mode::Existing);
    REQUIRE(storage);
    const auto read = read_all(*storage, /*repair=*/true).value();
    CHECK(all_intact(read, 100));
    CHECK(read.report.blocks_repaired == 1);
    CHECK(verify(*storage)->clean());
    std::filesystem::remove(path);
}

TEST_CASE("свойство: любые повреждения не больше m блоков в полосе не теряют ни одной записи (случайные прогоны)") {
    Math::Rng rng(2024);
    for (int round = 0; round < 60; ++round) {
        MemoryStorage storage;
        const int count = 100 + static_cast<int>(rng.below(300));
        fill(storage, count);
        const std::uint64_t stripes = verify(storage)->stripes;
        std::uint64_t damaged = 0;
        for (std::size_t s = 0; s < stripes; ++s) {
            const int bad = static_cast<int>(rng.below(3)); // 0, 1 или 2 блока (m = 2)
            std::set<std::size_t> chosen;
            while (static_cast<int>(chosen.size()) < bad) chosen.insert(rng.below(6));
            for (const std::size_t b : chosen) corrupt_block(storage, s, b, rng.next_u32());
            damaged += chosen.size();
        }
        const auto read = read_all(storage).value();
        REQUIRE(all_intact(read, count));
        REQUIRE(read.report.blocks_repaired == damaged);
    }
}

TEST_CASE("свойство: при любом разрушении читатель не падает и не возвращает недостоверных записей") {
    Math::Rng rng(77);
    for (int round = 0; round < 60; ++round) {
        MemoryStorage storage;
        fill(storage, 250);
        auto& bytes = storage.bytes();
        const int flips = static_cast<int>(rng.below(static_cast<std::uint32_t>(bytes.size() / 8)));
        for (int i = 0; i < flips; ++i) bytes[base + rng.below(static_cast<std::uint32_t>(bytes.size() - base))] ^= static_cast<std::byte>(1 + rng.below(255));
        if (rng.below(4) == 0) bytes.resize(base + rng.below(static_cast<std::uint32_t>(bytes.size() - base))); // ещё и обрыв
        const auto read = read_all(storage);
        REQUIRE(read.has_value()); // заголовок цел (портим после него)
        std::uint64_t previous = 0;
        bool first = true;
        for (const Record& r : read->records) {
            REQUIRE(r.sequence < 250);
            REQUIRE(r.payload == payload_for(r.sequence, r.sequence * 37 + 5)); // достоверность: чужих и «почти правильных» записей нет
            if (!first) REQUIRE(r.sequence > previous);
            previous = r.sequence;
            first = false;
        }
        REQUIRE(read->report.records + read->report.records_lost <= 250);
    }
}
