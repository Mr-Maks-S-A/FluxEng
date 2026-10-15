#include <AudioSystem/Wave.hpp>

#include <doctest/doctest.h>

#include <cmath>

using namespace AudioSystem;

TEST_CASE("make_tone: длина, формат и амплитуда") {
    const Wave w = make_tone(Shape::Sine, 440.0f, 0.5f, 48000, 0.5f);
    CHECK(w.channels == 1);
    CHECK(w.frames() == 24000);
    CHECK(w.seconds() == doctest::Approx(0.5));
    float peak = 0.0f;
    for (float s : w.samples) peak = std::max(peak, std::fabs(s));
    CHECK(peak == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(w.samples.back() == doctest::Approx(0.0f).epsilon(0.05)); // затухание в конце: без щелчка
}

TEST_CASE("make_tone: частота синуса по переходам через ноль") {
    const Wave w = make_tone(Shape::Sine, 1000.0f, 1.0f, 48000, 0.8f);
    int crossings = 0;
    for (std::size_t i = 1; i < w.samples.size(); ++i)
        if (w.samples[i - 1] < 0.0f && w.samples[i] >= 0.0f) ++crossings;
    CHECK(crossings == doctest::Approx(1000).epsilon(0.01));
}

TEST_CASE("шум детерминирован по seed") {
    const Wave a = make_noise_burst(0.1f, 10.0f, 48000, 0.6f, 7);
    const Wave b = make_noise_burst(0.1f, 10.0f, 48000, 0.6f, 7);
    const Wave c = make_noise_burst(0.1f, 10.0f, 48000, 0.6f, 8);
    CHECK(a.samples == b.samples);
    CHECK(a.samples != c.samples);
    CHECK(std::fabs(a.samples.front()) > std::fabs(a.samples.back()));
}

TEST_CASE("WAV: запись и чтение возвращают тот же звук (с точностью 16 бит)") {
    Wave w = make_tone(Shape::Triangle, 330.0f, 0.1f, 22050, 0.7f);
    w.channels = 2; // сделаем стерео: каждый кадр — два соседних отсчёта
    w.samples.resize(w.samples.size() / 2 * 2);
    const auto bytes = encode_wav(w);
    REQUIRE(bytes.size() == 44 + w.samples.size() * 2);
    const auto back = decode_wav(bytes);
    REQUIRE(back);
    CHECK(back->channels == 2);
    CHECK(back->sample_rate == 22050);
    REQUIRE(back->samples.size() == w.samples.size());
    for (std::size_t i = 0; i < w.samples.size(); ++i) CHECK(back->samples[i] == doctest::Approx(w.samples[i]).epsilon(0.0005).scale(1.0));
}

TEST_CASE("WAV: ошибки — сообщением, не падением") {
    CHECK_FALSE(decode_wav({}));
    const std::vector<std::uint8_t> junk(64, 0x42);
    CHECK_FALSE(decode_wav(junk));
    auto bytes = encode_wav(make_tone(Shape::Sine, 100.0f, 0.01f));
    auto cut = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 30); // обрезан внутри fmt
    CHECK_FALSE(decode_wav(cut));
    bytes[20] = 99; // неизвестный формат
    CHECK_FALSE(decode_wav(bytes));
    CHECK_FALSE(load_wav("/nonexistent/none.wav"));
}

TEST_CASE("WAV: оборванный data-чанк читается, сколько есть") {
    auto bytes = encode_wav(make_tone(Shape::Sine, 100.0f, 0.02f, 8000));
    bytes.resize(bytes.size() - 7);
    const auto w = decode_wav(bytes);
    REQUIRE(w);
    CHECK(w->frames() > 0);
}
