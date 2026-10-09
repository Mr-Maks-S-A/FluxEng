#include <NetSystem/NetSystem.hpp>

#include <doctest/doctest.h>

#include <cmath>

using namespace NetSystem;

namespace {
struct Pod {
    std::int32_t a;
    std::uint16_t b;
    std::uint16_t c;
};
enum class Kind : std::uint8_t { A = 1, B = 7 };
} // namespace

TEST_SUITE("NetSystem.Bytes") {

TEST_CASE("запись и чтение: все типы туда и обратно") {
    ByteWriter w;
    w.write<std::uint8_t>(200).write<std::int16_t>(-1234).write<std::uint32_t>(0xDEADBEEF).write<std::int64_t>(-5'000'000'000ll);
    w.write<float>(1.5f).write<double>(-2.25).write(Kind::B).write(Pod{-7, 8, 9});
    const auto bytes = w.take();

    ByteReader r(bytes);
    CHECK(r.read<std::uint8_t>() == 200);
    CHECK(r.read<std::int16_t>() == -1234);
    CHECK(r.read<std::uint32_t>() == 0xDEADBEEF);
    CHECK(r.read<std::int64_t>() == -5'000'000'000ll);
    CHECK(r.read<float>() == 1.5f);
    CHECK(r.read<double>() == -2.25);
    CHECK(r.read<Kind>() == Kind::B);
    const Pod pod = r.read<Pod>();
    CHECK(pod.a == -7);
    CHECK(pod.c == 9);
    CHECK(r.ok());
    CHECK(r.finished());
}

TEST_CASE("порядок байт — little-endian на любой платформе") {
    ByteWriter w;
    w.write<std::uint32_t>(0x01020304u);
    const auto bytes = w.bytes();
    REQUIRE(bytes.size() == 4);
    CHECK(bytes[0] == std::byte{4});
    CHECK(bytes[3] == std::byte{1});
}

TEST_CASE("чтение за границей: нули и флаг ошибки, без падения; ошибка «липкая»") {
    ByteWriter w;
    w.write<std::uint16_t>(5);
    const auto bytes = w.take();
    ByteReader r(bytes);
    CHECK(r.read<std::uint32_t>() == 0); // нужно 4 байта, есть 2
    CHECK_FALSE(r.ok());
    CHECK(r.read<std::uint8_t>() == 0);  // после ошибки читать уже нельзя, даже если байты остались
    CHECK_FALSE(r.finished());
    CHECK(r.read_bytes(1).empty());
}

TEST_CASE("read_bytes и finished") {
    ByteWriter w;
    w.write<std::uint8_t>(1).write<std::uint8_t>(2).write<std::uint8_t>(3);
    const auto bytes = w.take();
    ByteReader r(bytes);
    CHECK(r.read_bytes(2).size() == 2);
    CHECK_FALSE(r.finished()); // остался байт
    CHECK(r.read_bytes(1).size() == 1);
    CHECK(r.finished());
    CHECK(r.remaining() == 0);
}

TEST_CASE("NaN и бесконечность проходят побитово") {
    ByteWriter w;
    w.write(std::numeric_limits<float>::infinity()).write(std::numeric_limits<float>::quiet_NaN());
    ByteReader r(w.bytes());
    CHECK(std::isinf(r.read<float>()));
    CHECK(std::isnan(r.read<float>()));
}

} // TEST_SUITE
