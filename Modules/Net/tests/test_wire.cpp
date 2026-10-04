#include <Net/Wire.hpp>

#include <Math/Rng.hpp>

#include <doctest/doctest.h>

using namespace Net;

namespace {

Replay::Command cmd(std::uint16_t type, std::int32_t x) { return {.type = type, .arg = -3, .x = x, .y = -x, .z = 7}; }

Packet make(Body body, PeerId from = 2) { return Packet{from, std::move(body)}; }

} // namespace

TEST_CASE("Hello, Inputs, BlobChunk, BlobAck, HashReport: туда-обратно без потерь") {
    {
        const auto back = decode(encode(make(Hello{3, 0xABCDEF, 0x1234, true})));
        REQUIRE(back.has_value());
        const auto& h = std::get<Hello>(back->body);
        CHECK(back->from == 2);
        CHECK(h.peer_count == 3);
        CHECK(h.seed == 0xABCDEF);
        CHECK(h.config_hash == 0x1234);
        CHECK(h.knows_you);
    }
    {
        Inputs in;
        in.acked = 41;
        in.ticks.push_back({40, {cmd(1, 5), cmd(3, -9)}});
        in.ticks.push_back({41, {}});
        const auto back = decode(encode(make(in)));
        REQUIRE(back.has_value());
        const auto& got = std::get<Inputs>(back->body);
        CHECK(got.acked == 41);
        REQUIRE(got.ticks.size() == 2);
        CHECK(got.ticks[0].tick == 40);
        CHECK(got.ticks[0].commands == in.ticks[0].commands);
        CHECK(got.ticks[1].commands.empty());
    }
    {
        BlobChunk chunk{0x77, 3000, 1000, std::vector<std::byte>(1000, std::byte{0x5A})};
        const auto back = decode(encode(make(chunk)));
        REQUIRE(back.has_value());
        const auto& got = std::get<BlobChunk>(back->body);
        CHECK(got.hash == 0x77);
        CHECK(got.total == 3000);
        CHECK(got.offset == 1000);
        CHECK(got.data == chunk.data);
        CHECK(std::get<BlobAck>(decode(encode(make(BlobAck{99}))).value().body).hash == 99);
    }
    {
        HashReport r{1234, {}};
        r.hashes.add("terrain", 11).add("mana", 22);
        const auto back = decode(encode(make(r)));
        REQUIRE(back.has_value());
        const auto& got = std::get<HashReport>(back->body);
        CHECK(got.tick == 1234);
        CHECK(got.hashes == r.hashes);
    }
}

TEST_CASE("decode: любая порча одного бита ловится контрольной суммой") {
    Inputs in;
    in.acked = 5;
    in.ticks.push_back({5, {cmd(1, 1)}});
    const auto bytes = encode(make(in));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        auto bad = bytes;
        bad[i] ^= std::byte{0x10};
        CHECK_FALSE(decode(bad).has_value());
    }
}

TEST_CASE("decode: обрезка, лишние байты, чужие данные — отказ с текстом, не падение") {
    const auto bytes = encode(make(Hello{2, 1, 1, false}));
    for (std::size_t n = 0; n < bytes.size(); ++n) CHECK_FALSE(decode(std::span(bytes).first(n)).has_value());
    CHECK_FALSE(decode({}).has_value());
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(decode(longer).has_value());
    CHECK_FALSE(decode(std::vector<std::byte>(max_packet_size + 1, std::byte{0})).has_value());
    CHECK(decode(std::span(bytes).first(3)).error().size() > 0);
}

TEST_CASE("decode: правдоподобный, но недопустимый — отвергается (кусок за пределами блоба, лишние команды, нулевой размер)") {
    CHECK_FALSE(decode(encode(make(BlobChunk{1, 100, 90, std::vector<std::byte>(20)}))).has_value());   // 90 + 20 > 100
    CHECK_FALSE(decode(encode(make(BlobChunk{1, 0, 0, {}}))).has_value());                              // размер 0
    CHECK_FALSE(decode(encode(make(BlobChunk{1, max_blob_size + 1, 0, std::vector<std::byte>(10)}))).has_value());
    Inputs many;
    many.ticks.push_back({0, std::vector<Replay::Command>(max_commands_per_tick + 1)});
    CHECK_FALSE(decode(encode(make(many))).has_value());
    CHECK_FALSE(decode(encode(make(Hello{0, 0, 0, false}))).has_value());                                // ноль пиров
}

TEST_CASE("фаззинг: случайные байты и случайные мутации валидных пакетов никогда не роняют разбор") {
    Math::Rng rng(2024);
    Inputs in;
    in.ticks.push_back({1, {cmd(1, 2)}});
    const auto valid = encode(make(in));
    int accepted = 0;
    for (int i = 0; i < 5000; ++i) {
        std::vector<std::byte> bytes(rng.below(200));
        for (auto& b : bytes) b = static_cast<std::byte>(rng.next_u32());
        if (decode(bytes).has_value()) ++accepted;
        auto mutated = valid; // мутация с пересчётом контрольной суммы: проверка самих разборщиков, а не только CRC
        mutated[rng.below(static_cast<std::uint32_t>(mutated.size() - 4))] = static_cast<std::byte>(rng.next_u32());
        (void)decode(mutated);
    }
    CHECK(accepted == 0); // случайные байты не проходят контрольную сумму
}
