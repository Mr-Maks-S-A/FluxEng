#include "TestAssets.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <random>

using namespace AssetSystem;
using testassets::bytes_of;

namespace {

PackWriter sample_writer() {
    PackWriter w;
    REQUIRE(w.add("models/a.fmesh", bytes_of("mesh-a-data")));
    REQUIRE(w.add("textures/b.png", bytes_of("png-bytes-here-0123456789")));
    REQUIRE(w.add("data/spells.json", bytes_of(R"({"fireball":{"damage":12}})")));
    return w;
}

} // namespace

TEST_SUITE("AssetSystem.Pack") {

TEST_CASE("запись и чтение пака: все файлы совпадают") {
    auto pack = PackSource::open(*sample_writer().build());
    REQUIRE(pack.has_value());
    CHECK((*pack)->entry_count() == 3);
    CHECK(*(*pack)->read("models/a.fmesh") == bytes_of("mesh-a-data"));
    CHECK(*(*pack)->read("data/spells.json") == bytes_of(R"({"fireball":{"damage":12}})"));
    CHECK_FALSE((*pack)->exists("nope"));
    CHECK((*pack)->read("nope").error().code == ErrorCode::NotFound);
}

TEST_CASE("пак воспроизводим: одинаковые входы — одинаковые байты") {
    CHECK(*sample_writer().build() == *sample_writer().build());
}

TEST_CASE("PackWriter отвергает дубликаты и плохие пути") {
    PackWriter w;
    CHECK(w.add("a.txt", bytes_of("1")));
    CHECK_FALSE(w.add("./a.txt", bytes_of("2"))); // тот же нормализованный путь
    CHECK_FALSE(w.add("../a.txt", bytes_of("3")));
    CHECK(w.entry_count() == 1);
}

TEST_CASE("пустой пак допустим") {
    PackWriter w;
    auto pack = PackSource::open(*w.build());
    REQUIRE(pack.has_value());
    CHECK((*pack)->entry_count() == 0);
}

TEST_CASE("порча данных ловится контрольной суммой при чтении записи") {
    Bytes bytes = *sample_writer().build();
    bytes[bytes.size() - 3] ^= std::byte{0x40}; // внутри данных последней записи
    auto pack = PackSource::open(bytes);
    REQUIRE(pack.has_value()); // таблица цела
    const auto r = (*pack)->read("data/spells.json");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::Corrupt);
    CHECK((*pack)->read("models/a.fmesh").has_value()); // соседние записи не пострадали
}

TEST_CASE("неверная магия, версия и размер таблицы") {
    Bytes bytes = *sample_writer().build();
    Bytes bad_magic = bytes;
    bad_magic[0] = std::byte{'X'};
    CHECK(PackSource::open(bad_magic).error().code == ErrorCode::Corrupt);

    Bytes bad_version = bytes;
    bad_version[4] = std::byte{9};
    CHECK(PackSource::open(bad_version).error().code == ErrorCode::Unsupported);

    Bytes huge_count = bytes;
    huge_count[8] = huge_count[9] = huge_count[10] = std::byte{0xFF}; // миллионы записей
    huge_count[11] = std::byte{0x7F};
    const auto r = PackSource::open(huge_count);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::TooLarge);

    CHECK_FALSE(PackSource::open(Bytes{}).has_value());
}

TEST_CASE("запись, указывающая за конец файла, отвергается при открытии") {
    Bytes bytes = *sample_writer().build();
    bytes.resize(bytes.size() - 10); // обрезали данные последней записи
    const auto r = PackSource::open(bytes);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::Corrupt);
}

TEST_CASE("любое усечение пака даёт ошибку или корректный результат — без падений") {
    const Bytes full = *sample_writer().build();
    for (std::size_t n = 0; n < full.size(); ++n) {
        CAPTURE(n);
        auto pack = PackSource::open(Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n)));
        if (pack) {
            std::vector<std::string> names;
            (*pack)->list(names);
            for (const std::string& name : names) (void)(*pack)->read(name);
        }
    }
}

TEST_CASE("случайная порча байтов: открытие и чтение всегда завершаются штатно") {
    const Bytes full = *sample_writer().build();
    std::mt19937 rng(12345);
    for (int round = 0; round < 3000; ++round) {
        Bytes bytes = full;
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f) bytes[rng() % bytes.size()] ^= static_cast<std::byte>(1u << (rng() % 8));
        auto pack = PackSource::open(std::move(bytes));
        if (!pack) continue;
        std::vector<std::string> names;
        (*pack)->list(names);
        for (const std::string& name : names) (void)(*pack)->read(name);
    }
}

} // TEST_SUITE
