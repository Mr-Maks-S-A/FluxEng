#include <AudioSystem/Mixer.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <vector>

using namespace AudioSystem;

namespace {

double rms(const std::vector<float>& v, std::size_t from, std::size_t to, int channel) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < to; ++i, ++n) sum += static_cast<double>(v[i * 2 + static_cast<std::size_t>(channel)]) * v[i * 2 + static_cast<std::size_t>(channel)];
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

std::vector<float> render(Mixer& m, std::size_t frames) {
    std::vector<float> out(frames * 2);
    m.render(out);
    return out;
}

} // namespace

TEST_CASE("midi_to_hz") {
    CHECK(midi_to_hz(69) == doctest::Approx(440.0f));
    CHECK(midi_to_hz(81) == doctest::Approx(880.0f));
    CHECK(midi_to_hz(60) == doctest::Approx(261.63f).epsilon(0.001));
}

TEST_CASE("тишина без голосов") {
    Mixer m(48000);
    for (float s : render(m, 256)) CHECK(s == 0.0f);
    CHECK(m.active_voices() == 0);
    CHECK(m.frames_rendered() == 256);
}

TEST_CASE("нота: звучит, держится, затухает и освобождает голос") {
    Mixer m(48000);
    const VoiceId id = m.note_on({.shape = Shape::Sine, .frequency = 440.0f, .volume = 0.5f, .envelope = {.attack = 0.001f, .decay = 0.0f, .sustain = 1.0f, .release = 0.05f}});
    REQUIRE(id);
    CHECK(m.is_active(id));
    const auto a = render(m, 4800);
    CHECK(rms(a, 480, 4800, 0) == doctest::Approx(0.5 / std::sqrt(2.0) * std::sqrt(0.5)).epsilon(0.05)); // синус × панорама по центру (cos 45°)
    CHECK(m.is_active(id));
    m.release(id);
    render(m, 4800); // 0,1 с > release 0,05 с
    CHECK_FALSE(m.is_active(id));
    CHECK(m.active_voices() == 0);
}

TEST_CASE("play_note сама отпускает ноту через hold") {
    Mixer m(48000);
    const VoiceId id = m.play_note({.frequency = 220.0f, .envelope = {.attack = 0.001f, .decay = 0.0f, .sustain = 1.0f, .release = 0.02f}}, 0.1f);
    render(m, 4000);
    CHECK(m.is_active(id));
    render(m, 3000); // 0,1 с + затухание 0,02 с
    CHECK_FALSE(m.is_active(id));
}

TEST_CASE("панорама: левый и правый каналы") {
    Mixer m(48000);
    m.note_on({.frequency = 440.0f, .pan = -1.0f, .envelope = {.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = 0.01f}});
    const auto out = render(m, 2000);
    CHECK(rms(out, 100, 2000, 0) > 0.1);
    CHECK(rms(out, 100, 2000, 1) < 0.001);
}

TEST_CASE("сэмпл: проигрывается один раз, зацикливание и питч") {
    auto wave = std::make_shared<Wave>(make_tone(Shape::Square, 100.0f, 0.1f, 48000, 0.5f));
    Mixer m(48000);
    const VoiceId once = m.play(wave);
    REQUIRE(once);
    render(m, 5000); // дольше сэмпла (4800)
    CHECK_FALSE(m.is_active(once));

    const VoiceId loop = m.play(wave, {.loop = true});
    render(m, 20000);
    CHECK(m.is_active(loop));
    m.stop(loop);
    CHECK_FALSE(m.is_active(loop));

    const VoiceId fast = m.play(wave, {.pitch = 2.0f});
    render(m, 2600); // вдвое быстрее: 2400 кадров
    CHECK_FALSE(m.is_active(fast));
}

TEST_CASE("недопустимый сэмпл не создаёт голос") {
    Mixer m;
    CHECK_FALSE(m.play(nullptr));
    CHECK_FALSE(m.play(std::make_shared<Wave>()));
}

TEST_CASE("заполненный микшер вытесняет самый старый голос; номера не повторяются") {
    Mixer m(48000, 4);
    std::vector<VoiceId> ids;
    for (int i = 0; i < 6; ++i) ids.push_back(m.note_on({.frequency = 200.0f + 50.0f * static_cast<float>(i)}));
    CHECK(m.active_voices() == 4);
    CHECK_FALSE(m.is_active(ids[0]));
    CHECK_FALSE(m.is_active(ids[1]));
    CHECK(m.is_active(ids[5]));
    for (std::size_t i = 1; i < ids.size(); ++i) CHECK(ids[i].value != ids[i - 1].value);
    m.stop(ids[0]); // устаревший номер: ничего не ломает
    m.release(ids[0]);
    CHECK(m.active_voices() == 4);
}

TEST_CASE("мастер-громкость и ограничение амплитуды") {
    Mixer m(48000);
    for (int i = 0; i < 8; ++i) m.note_on({.shape = Shape::Square, .frequency = 100.0f, .volume = 1.0f, .envelope = {.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = 0.01f}});
    for (float s : render(m, 1000)) CHECK(std::fabs(s) <= 1.0f);
    m.set_master_volume(0.0f);
    for (float s : render(m, 100)) CHECK(s == 0.0f);
}

TEST_CASE("детерминизм: два микшера с теми же командами дают те же отсчёты, включая шум и вибрато") {
    auto run = [] {
        Mixer m(44100);
        m.note_on({.shape = Shape::Noise, .volume = 0.4f});
        m.play_note({.shape = Shape::Saw, .frequency = 330.0f, .vibrato_hz = 5.0f, .vibrato_depth = 0.3f}, 0.05f);
        return render(m, 3000);
    };
    CHECK(run() == run());
}

TEST_CASE("разбиение на блоки не влияет на результат") {
    auto make = [] {
        Mixer m(48000);
        m.note_on({.shape = Shape::Triangle, .frequency = 261.6f, .volume = 0.5f});
        return m;
    };
    Mixer a = make(), b = make();
    const auto whole = render(a, 3000);
    std::vector<float> parts;
    for (std::size_t n : {1u, 7u, 512u, 1000u, 1480u}) {
        const auto p = render(b, n);
        parts.insert(parts.end(), p.begin(), p.end());
    }
    CHECK(whole == parts);
}
