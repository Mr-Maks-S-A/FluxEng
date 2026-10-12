#include <AudioSystem/AudioSystem.hpp>

#include "TestOgg.hpp"
#include "TestSupport.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cstring>
#include <random>

using namespace AudioSystem;

namespace {

std::span<const std::byte> as_bytes(const std::vector<std::byte>& v) { return {v.data(), v.size()}; }

void put_u16(std::vector<std::byte>& out, std::uint16_t v) { for (int i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF)); }
void put_u32(std::vector<std::byte>& out, std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF)); }
void put_tag(std::vector<std::byte>& out, const char* t) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(t[i])); }

/// Собирает WAV вручную: любые формат/биты/каналы и произвольные байты данных.
std::vector<std::byte> make_wav(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits, const std::vector<std::byte>& data,
                                std::uint32_t declared_data_size = 0xFFFFFFFEu) {
    std::vector<std::byte> out;
    const auto align = static_cast<std::uint16_t>(channels * bits / 8);
    put_tag(out, "RIFF"); put_u32(out, 0); put_tag(out, "WAVE");
    put_tag(out, "fmt "); put_u32(out, 16);
    put_u16(out, format); put_u16(out, channels); put_u32(out, rate); put_u32(out, rate * align); put_u16(out, align); put_u16(out, bits);
    put_tag(out, "data");
    put_u32(out, declared_data_size == 0xFFFFFFFEu ? static_cast<std::uint32_t>(data.size()) : declared_data_size);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

} // namespace

TEST_SUITE("AudioSystem.Clip") {

TEST_CASE("WAV 16 бит: запись и чтение возвращают сигнал с точностью до кванта") {
    const AudioClip sine = make_sine(440.0f, 0.05, 44'100, 0.8f);
    const auto bytes = encode_wav(sine);
    const auto decoded = decode_wav(as_bytes(bytes));
    REQUIRE(decoded);
    CHECK(decoded->sample_rate == 44'100);
    CHECK(decoded->channels == 1);
    REQUIRE(decoded->samples.size() == sine.samples.size());
    for (std::size_t i = 0; i < sine.samples.size(); ++i) CHECK(std::abs(decoded->samples[i] - sine.samples[i]) < 1.0f / 16000.0f);
}

TEST_CASE("WAV float32: данные возвращаются бит в бит, стерео сохраняется") {
    AudioClip clip;
    clip.sample_rate = 48'000;
    clip.channels = 2;
    clip.samples = {0.25f, -0.5f, 1.5f, -2.0f, 0.0f, 0.125f};
    const auto decoded = decode_wav(as_bytes(encode_wav(clip, WavFormat::Float32)));
    REQUIRE(decoded);
    CHECK(decoded->channels == 2);
    CHECK(decoded->samples == clip.samples);
}

TEST_CASE("точки повтора переживают запись в smpl") {
    AudioClip clip = make_sine(100.0f, 0.1, 48'000);
    clip.loop_begin = 1000;
    clip.loop_end = 3000;
    const auto decoded = decode_wav(as_bytes(encode_wav(clip)));
    REQUIRE(decoded);
    CHECK(decoded->loop_begin == 1000);
    CHECK(decoded->loop_end == 3000);
    CHECK(decoded->effective_loop_end() == 3000);
}

TEST_CASE("разрядности PCM: 8, 24 и 32 бита, float64") {
    SUBCASE("8 бит: 128 — тишина, 255 — почти +1") {
        const auto wav = decode_wav(as_bytes(make_wav(1, 1, 8000, 8, {std::byte{128}, std::byte{255}, std::byte{0}})));
        REQUIRE(wav);
        CHECK(wav->samples[0] == doctest::Approx(0.0f));
        CHECK(wav->samples[1] == doctest::Approx(127.0f / 128.0f));
        CHECK(wav->samples[2] == doctest::Approx(-1.0f));
    }
    SUBCASE("24 бита со знаком") {
        std::vector<std::byte> d;
        // +0.5 = 0x400000, −0.5 = 0xC00000
        for (std::uint32_t v : {0x400000u, 0xC00000u}) { d.push_back(static_cast<std::byte>(v & 0xFF)); d.push_back(static_cast<std::byte>((v >> 8) & 0xFF)); d.push_back(static_cast<std::byte>((v >> 16) & 0xFF)); }
        const auto wav = decode_wav(as_bytes(make_wav(1, 1, 8000, 24, d)));
        REQUIRE(wav);
        CHECK(wav->samples[0] == doctest::Approx(0.5f));
        CHECK(wav->samples[1] == doctest::Approx(-0.5f));
    }
    SUBCASE("32 бита со знаком") {
        std::vector<std::byte> d;
        put_u32(d, 0x40000000u);
        put_u32(d, 0x80000000u);
        const auto wav = decode_wav(as_bytes(make_wav(1, 1, 8000, 32, d)));
        REQUIRE(wav);
        CHECK(wav->samples[0] == doctest::Approx(0.5f));
        CHECK(wav->samples[1] == doctest::Approx(-1.0f));
    }
    SUBCASE("float64") {
        std::vector<std::byte> d;
        const auto bits = std::bit_cast<std::uint64_t>(0.75);
        put_u32(d, static_cast<std::uint32_t>(bits & 0xFFFFFFFFu));
        put_u32(d, static_cast<std::uint32_t>(bits >> 32));
        const auto wav = decode_wav(as_bytes(make_wav(3, 1, 8000, 64, d)));
        REQUIRE(wav);
        CHECK(wav->samples[0] == doctest::Approx(0.75f));
    }
}

TEST_CASE("многоканальный WAV сводится к стерео") {
    std::vector<std::byte> d;
    for (int frame = 0; frame < 4; ++frame) {
        for (int ch = 0; ch < 6; ++ch) put_u16(d, static_cast<std::uint16_t>(static_cast<std::int16_t>(ch % 2 == 0 ? 16384 : -16384)));
    }
    const auto wav = decode_wav(as_bytes(make_wav(1, 6, 48'000, 16, d)));
    REQUIRE(wav);
    CHECK(wav->channels == 2);
    CHECK(wav->frames() == 4);
    CHECK(wav->samples[0] == doctest::Approx(0.5f));
    CHECK(wav->samples[1] == doctest::Approx(-0.5f));
}

TEST_CASE("враждебные WAV: отказ с причиной, без падения") {
    const std::vector<std::byte> one_frame = {std::byte{0}, std::byte{0}};
    SUBCASE("пусто и мусор") {
        CHECK_FALSE(decode_wav({}));
        const std::vector<std::byte> junk(100, std::byte{0x41});
        CHECK_FALSE(decode_wav(as_bytes(junk)));
    }
    SUBCASE("нет блока fmt") {
        std::vector<std::byte> w;
        put_tag(w, "RIFF"); put_u32(w, 0); put_tag(w, "WAVE"); put_tag(w, "data"); put_u32(w, 2); put_u16(w, 1);
        CHECK_FALSE(decode_wav(as_bytes(w)));
    }
    SUBCASE("нулевая частота и ноль/слишком много каналов") {
        CHECK_FALSE(decode_wav(as_bytes(make_wav(1, 1, 0, 16, one_frame))));
        CHECK_FALSE(decode_wav(as_bytes(make_wav(1, 0, 8000, 16, one_frame))));
        CHECK_FALSE(decode_wav(as_bytes(make_wav(1, 9, 8000, 16, std::vector<std::byte>(18, std::byte{0})))));
    }
    SUBCASE("неподдерживаемый формат (ADPCM) и разрядность") {
        CHECK_FALSE(decode_wav(as_bytes(make_wav(2, 1, 8000, 16, one_frame))));
        CHECK_FALSE(decode_wav(as_bytes(make_wav(1, 1, 8000, 12, one_frame))));
    }
    SUBCASE("размер data врёт: берётся то, что есть") {
        const auto wav = decode_wav(as_bytes(make_wav(1, 1, 8000, 16, std::vector<std::byte>(8, std::byte{0}), 0xFFFFFFF0u)));
        REQUIRE(wav);
        CHECK(wav->frames() == 4);
    }
    SUBCASE("предел длины проверяется до выделения") {
        DecodeLimits limits;
        limits.max_frames = 3;
        CHECK_FALSE(decode_wav(as_bytes(make_wav(1, 1, 8000, 16, std::vector<std::byte>(8, std::byte{0}))), limits));
    }
    SUBCASE("блок fmt больше файла") {
        auto w = make_wav(1, 1, 8000, 16, one_frame);
        w.resize(30);
        CHECK_FALSE(decode_wav(as_bytes(w)));
    }
}

TEST_CASE("порча байтов корректного WAV никогда не роняет декодер") {
    const auto good = encode_wav(make_sine(300.0f, 0.02, 22'050));
    std::mt19937 rng(42);
    for (int round = 0; round < 3000; ++round) {
        auto bad = good;
        for (int k = 0; k < 4; ++k) bad[rng() % bad.size()] = static_cast<std::byte>(rng());
        if (round % 7 == 0) bad.resize(rng() % bad.size());
        const auto result = decode_wav(as_bytes(bad));
        if (result) CHECK(result->valid());
    }
}

TEST_CASE("OGG Vorbis: тестовый синус декодируется") {
    std::vector<std::byte> bytes(TestOgg::tone.size());
    std::memcpy(bytes.data(), TestOgg::tone.data(), bytes.size());
    const auto ogg = decode_ogg(as_bytes(bytes));
    REQUIRE(ogg);
    CHECK(ogg->sample_rate == 22'050);
    CHECK(ogg->channels == 1);
    CHECK(ogg->frames() > 1500);
    CHECK(ogg->frames() < 3000);
    // Синус амплитудой 0.125 → RMS ≈ 0.088 (кодек теряет немного).
    double sum = 0.0;
    for (std::size_t i = 200; i < 1500; ++i) sum += static_cast<double>(ogg->samples[i]) * ogg->samples[i];
    const double level = std::sqrt(sum / 1300.0);
    CHECK(level > 0.06);
    CHECK(level < 0.12);
}

TEST_CASE("decode_audio выбирает декодер по сигнатуре") {
    std::vector<std::byte> ogg(TestOgg::tone.size());
    std::memcpy(ogg.data(), TestOgg::tone.data(), ogg.size());
    CHECK(decode_audio(as_bytes(ogg)));
    CHECK(decode_audio(as_bytes(encode_wav(make_sine(100.0f, 0.01)))));
    const std::vector<std::byte> junk(64, std::byte{1});
    CHECK_FALSE(decode_audio(as_bytes(junk)));
}

TEST_CASE("OGG: мусор и оборванная загрузка дают ошибку или короткий клип, без падения и утечек") {
    // Испорченные байты внутри заголовков stb_vorbis не выдерживает (см. предупреждение у decode_ogg), поэтому проверяем честную обрезку.
    std::vector<std::byte> good(TestOgg::tone.size());
    std::memcpy(good.data(), TestOgg::tone.data(), good.size());
    CHECK_FALSE(decode_ogg({}));
    const std::vector<std::byte> not_ogg(64, std::byte{1});
    CHECK_FALSE(decode_ogg(as_bytes(not_ogg)));
    for (std::size_t length = 0; length < good.size(); length += 3) {
        const auto result = decode_ogg(std::span<const std::byte>(good.data(), length));
        if (result) CHECK(result->valid());
    }
}

TEST_CASE("генераторы: шум воспроизводим и ограничен, константа — константа") {
    const AudioClip a = make_noise(0.01, 48'000, 5, 0.5f);
    const AudioClip b = make_noise(0.01, 48'000, 5, 0.5f);
    const AudioClip c = make_noise(0.01, 48'000, 6, 0.5f);
    CHECK(a.samples == b.samples);
    CHECK(a.samples != c.samples);
    for (const float s : a.samples) CHECK(std::abs(s) <= 0.5f);
    const AudioClip k = make_constant(0.25f, 10, 8000, 2);
    CHECK(k.frames() == 10);
    CHECK(k.samples.size() == 20);
    CHECK(k.valid());
    CHECK_FALSE(AudioClip{}.valid());
}

} // TEST_SUITE
