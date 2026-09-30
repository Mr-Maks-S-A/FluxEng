#include <MemorySystem/VirtualMemory.hpp>
#include <MemorySystem/Core.hpp>

#include <doctest/doctest.h>

#include <cstring>
#include <utility>

namespace ms = MemorySystem;

TEST_SUITE("MemorySystem.VirtualMemory") {

TEST_CASE("размер страницы и гранулярность — степени двойки") {
    CHECK(ms::is_power_of_two(ms::page_size()));
    CHECK(ms::is_power_of_two(ms::reservation_granularity()));
    CHECK(ms::reservation_granularity() >= ms::page_size());
}

TEST_CASE("пустой регион валиден (ZII)") {
    ms::VirtualRegion region;
    CHECK_FALSE(region);
    CHECK(region.data() == nullptr);
    CHECK(region.reserved() == 0);
    CHECK_FALSE(region.commit(0, 16));
}

TEST_CASE("резерв большого диапазона почти бесплатен, подтверждённые страницы нулевые") {
    ms::VirtualRegion region = ms::VirtualRegion::reserve(ms::GiB(1));
    REQUIRE(region);
    CHECK(region.reserved() >= ms::GiB(1));

    const std::size_t page = ms::page_size();
    REQUIRE(region.commit(page * 10, page * 2));
    std::byte* p = region.data() + page * 10;
    for (std::size_t i = 0; i < page * 2; ++i) {
        REQUIRE(p[i] == std::byte{0});
    }
    std::memset(p, 0xAB, page * 2);
    CHECK(p[page] == std::byte{0xAB});
}

TEST_CASE("decommit + commit возвращает нулевые страницы") {
    ms::VirtualRegion region = ms::VirtualRegion::reserve(ms::MiB(1));
    const std::size_t page = ms::page_size();
    REQUIRE(region.commit(0, page));
    std::memset(region.data(), 0x5A, page);
    region.decommit(0, page);
    REQUIRE(region.commit(0, page));
    CHECK(region.data()[0] == std::byte{0});
    CHECK(region.data()[page - 1] == std::byte{0});
}

TEST_CASE("commit за пределами резерва отклоняется") {
    ms::VirtualRegion region = ms::VirtualRegion::reserve(ms::KiB(64));
    CHECK_FALSE(region.commit(region.reserved(), 1));
    CHECK_FALSE(region.commit(0, region.reserved() + 1));
}

TEST_CASE("перемещение передаёт владение") {
    ms::VirtualRegion a = ms::VirtualRegion::reserve(ms::KiB(64));
    std::byte* base = a.data();
    ms::VirtualRegion b = std::move(a);
    CHECK_FALSE(a);
    CHECK(b.data() == base);
}

}
