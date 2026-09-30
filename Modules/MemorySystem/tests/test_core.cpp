#include <MemorySystem/Core.hpp>

#include <doctest/doctest.h>

#include <string>

namespace ms = MemorySystem;

TEST_SUITE("MemorySystem.Core") {

TEST_CASE("размеры") {
    static_assert(ms::KiB(1) == 1024);
    static_assert(ms::MiB(2) == 2u * 1024u * 1024u);
    static_assert(ms::GiB(1) == 1024u * 1024u * 1024u);
    CHECK(ms::KiB(64) == 65536);
}

TEST_CASE("align_up и is_power_of_two") {
    static_assert(ms::align_up(0, 16) == 0);
    static_assert(ms::align_up(1, 16) == 16);
    static_assert(ms::align_up(16, 16) == 16);
    static_assert(ms::align_up(17, 8) == 24);
    CHECK(ms::is_power_of_two(64));
    CHECK_FALSE(ms::is_power_of_two(0));
    CHECK_FALSE(ms::is_power_of_two(48));
}

TEST_CASE("ZeroInitializable: тривиальные типы — да, владеющие ресурсами — нет") {
    struct Vec2 {
        float x, y;
    };
    struct Handle {
        std::uint32_t index, generation;
    };
    static_assert(ms::ZeroInitializable<int>);
    static_assert(ms::ZeroInitializable<Vec2>);
    static_assert(ms::ZeroInitializable<Handle>);
    static_assert(ms::ZeroInitializable<Vec2[4]>);
    static_assert(!ms::ZeroInitializable<std::string>);
    CHECK(true);
}

TEST_CASE("start_lifetime_as_array не меняет байты") {
    alignas(int) unsigned char raw[sizeof(int) * 4] = {};
    raw[0] = 7;
    int* values = ms::start_lifetime_as_array<int>(raw, 4);
    CHECK(values[0] != 0);
    CHECK(values[1] == 0);
    CHECK(ms::start_lifetime_as_array<int>(nullptr, 4) == nullptr);
}

}
