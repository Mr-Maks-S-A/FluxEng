#include <EventLog/ErasureCode.hpp>

#include <Math/Rng.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <tuple>
#include <vector>

using namespace EventLog;

namespace {

struct Stripe {
    std::vector<std::vector<std::byte>> blocks; // k данных + m чётности
    std::vector<std::span<std::byte>> spans() {
        std::vector<std::span<std::byte>> out;
        for (auto& b : blocks) out.emplace_back(b);
        return out;
    }
};

Stripe make(const ErasureCode& code, std::size_t size, std::uint64_t seed) {
    Math::Rng rng(seed);
    Stripe s;
    s.blocks.assign(static_cast<std::size_t>(code.data_blocks() + code.parity_blocks()), std::vector<std::byte>(size));
    for (int i = 0; i < code.data_blocks(); ++i)
        for (std::byte& b : s.blocks[static_cast<std::size_t>(i)]) b = static_cast<std::byte>(rng.next_u32());
    std::vector<std::span<const std::byte>> data;
    std::vector<std::span<std::byte>> parity;
    for (int i = 0; i < code.data_blocks(); ++i) data.emplace_back(s.blocks[static_cast<std::size_t>(i)]);
    for (int j = 0; j < code.parity_blocks(); ++j) parity.emplace_back(s.blocks[static_cast<std::size_t>(code.data_blocks() + j)]);
    code.encode(data, parity);
    return s;
}

} // namespace

TEST_CASE("GF(2^8): умножение, обратный элемент, ассоциативность") {
    for (int a = 1; a < 256; ++a) {
        const auto x = static_cast<unsigned char>(a);
        CHECK(gf256::mul(x, gf256::inv(x)) == 1);
        CHECK(gf256::mul(x, 1) == x);
        CHECK(gf256::mul(x, 0) == 0);
    }
    CHECK(gf256::mul(2, 128) == 0x1D); // x · x⁷ = x⁸ = x⁴ + x³ + x² + 1 по модулю 0x11D
    for (int a : {3, 77, 200})
        for (int b : {5, 99, 251})
            for (int c : {7, 123, 254}) {
                const auto x = static_cast<unsigned char>(a), y = static_cast<unsigned char>(b), z = static_cast<unsigned char>(c);
                CHECK(gf256::mul(gf256::mul(x, y), z) == gf256::mul(x, gf256::mul(y, z)));
                CHECK(gf256::mul(x, static_cast<unsigned char>(y ^ z)) == (gf256::mul(x, y) ^ gf256::mul(x, z))); // распределительность
            }
}

TEST_CASE("восстановление любых m стёртых блоков: исчерпывающий перебор для k=5, m=3") {
    const ErasureCode code(5, 3);
    const Stripe original = make(code, 64, 1);
    const int n = 8;
    int cases = 0;
    for (int mask = 0; mask < (1 << n); ++mask) {
        int lost = 0;
        for (int b = 0; b < n; ++b) lost += (mask >> b) & 1;
        if (lost == 0 || lost > 3) continue;
        Stripe damaged = original;
        std::array<bool, 8> present{};
        for (int b = 0; b < n; ++b) {
            present[static_cast<std::size_t>(b)] = !((mask >> b) & 1);
            if (!present[static_cast<std::size_t>(b)]) std::ranges::fill(damaged.blocks[static_cast<std::size_t>(b)], std::byte{0xEE}); // мусор на месте потери
        }
        auto spans = damaged.spans();
        REQUIRE(code.reconstruct(spans, present));
        REQUIRE(damaged.blocks == original.blocks);
        ++cases;
    }
    CHECK(cases == 56 + 28 + 8); // C(8,1) + C(8,2) + C(8,3)
}

TEST_CASE("больше m потерь не восстанавливается и данные не портятся") {
    const ErasureCode code(4, 2);
    Stripe s = make(code, 32, 2);
    const Stripe before = s;
    std::array<bool, 6> present{false, false, false, true, true, true}; // три потери при m = 2
    auto spans = s.spans();
    CHECK_FALSE(code.reconstruct(spans, present));
    CHECK(s.blocks == before.blocks);
}

TEST_CASE("без потерь — ничего не меняется; разные k, m и размеры блоков, в том числе k = 1") {
    for (const auto& [k, m, size] : {std::tuple{1, 1, 1}, {1, 2, 17}, {2, 1, 100}, {8, 2, 4096}, {10, 4, 33}, {200, 55, 8}}) {
        const ErasureCode code(k, m);
        Stripe s = make(code, static_cast<std::size_t>(size), 3);
        const Stripe original = s;
        std::unique_ptr<bool[]> present(new bool[static_cast<std::size_t>(k) + static_cast<std::size_t>(m)]);
        for (int i = 0; i < k + m; ++i) present[static_cast<std::size_t>(i)] = true;
        auto spans = s.spans();
        CHECK(code.reconstruct(spans, std::span<const bool>(present.get(), static_cast<std::size_t>(k + m))));
        CHECK(s.blocks == original.blocks);
        // m потерь: самые «неудобные» — первые m блоков данных.
        for (int i = 0; i < std::min(m, k); ++i) {
            present[static_cast<std::size_t>(i)] = false;
            std::ranges::fill(s.blocks[static_cast<std::size_t>(i)], std::byte{0});
        }
        CHECK(code.reconstruct(spans, std::span<const bool>(present.get(), static_cast<std::size_t>(k + m))));
        CHECK(s.blocks == original.blocks);
    }
}

TEST_CASE("чётность определяется данными: другой набор данных — другая чётность") {
    const ErasureCode code(4, 2);
    const Stripe a = make(code, 64, 10), b = make(code, 64, 11);
    CHECK(a.blocks[4] != b.blocks[4]);
    CHECK(a.blocks[5] != b.blocks[5]);
    CHECK(make(code, 64, 10).blocks == a.blocks); // детерминированно
}
