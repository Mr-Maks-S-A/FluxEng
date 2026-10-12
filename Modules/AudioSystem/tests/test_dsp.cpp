#include <AudioSystem/AudioSystem.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

using namespace AudioSystem;

TEST_SUITE("AudioSystem.Dsp") {

TEST_CASE("Ramp идёт к цели ровно за заданное время") {
    Ramp r = Ramp::at(0.0f);
    r.set(1.0f, 0.01f, 48'000); // 480 кадров
    r.advance(240);
    CHECK(r.value == doctest::Approx(0.5f));
    CHECK_FALSE(r.settled());
    r.advance(1000);
    CHECK(r.value == 1.0f);
    CHECK(r.settled());
    r.set(0.25f, 0.0f, 48'000); // нулевое время — прыжок
    CHECK(r.value == 0.25f);
    r.set(0.0f, 0.01f, 48'000);
    r.advance(100);
    CHECK(r.value < 0.25f);
    CHECK(r.value > 0.0f);
}

TEST_CASE("LowPass: низкая частота проходит, высокая гасится") {
    const auto measure = [](float tone_hz, float cutoff_hz) {
        LowPass lp;
        lp.set_cutoff(cutoff_hz, 48'000);
        double energy_in = 0.0, energy_out = 0.0;
        for (int i = 0; i < 48'000; ++i) {
            const float x = std::sin(2.0f * std::numbers::pi_v<float> * tone_hz * static_cast<float>(i) / 48'000.0f);
            const float y = lp.process(x);
            if (i > 4800) { energy_in += static_cast<double>(x) * x; energy_out += static_cast<double>(y) * y; }
        }
        return std::sqrt(energy_out / energy_in);
    };
    CHECK(measure(100.0f, 1000.0f) > 0.98);
    CHECK(measure(10'000.0f, 1000.0f) < 0.02);
    CHECK(measure(1000.0f, 1000.0f) == doctest::Approx(0.707).epsilon(0.05)); // −3 дБ на срезе
}

TEST_CASE("постоянная мощность панорамы: сумма квадратов — 1") {
    for (float pan = -1.0f; pan <= 1.0f; pan += 0.125f) {
        const PanGains g = pan_constant_power(pan);
        CHECK(g.left * g.left + g.right * g.right == doctest::Approx(1.0f));
    }
    const PanGains center = pan_constant_power(0.0f);
    CHECK(center.left == doctest::Approx(0.70710678f));
    CHECK(center.right == doctest::Approx(0.70710678f));
    CHECK(pan_constant_power(-1.0f).right == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(pan_constant_power(1.0f).left == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(pan_constant_power(-5.0f).left == doctest::Approx(1.0f)); // вне диапазона зажимается
}

TEST_CASE("hermite: в узле точное значение, на прямой — линейно") {
    CHECK(hermite(0.0f, 9.0f, 1.0f, 2.0f, 9.0f) == 1.0f);
    CHECK(hermite(0.5f, 0.0f, 1.0f, 2.0f, 3.0f) == doctest::Approx(1.5f));
    CHECK(hermite(0.25f, 0.0f, 1.0f, 2.0f, 3.0f) == doctest::Approx(1.25f));
}

TEST_CASE("Reverb: импульс даёт хвост, выключенный — тишину; хвост затухает и конечен") {
    Reverb reverb(48'000);
    CHECK_FALSE(reverb.active());
    reverb.set({.room_size = 0.8f, .damping = 0.3f, .wet = 0.5f, .width = 1.0f});
    CHECK(reverb.active());

    const std::uint32_t n = 48'000 * 2;
    std::vector<float> in(n, 0.0f), zero(n, 0.0f), out_l(n), out_r(n);
    in[0] = 1.0f;
    reverb.process(in.data(), zero.data(), out_l.data(), out_r.data(), n);

    const auto energy = [&](std::size_t a, std::size_t b) {
        double e = 0.0;
        for (std::size_t i = a; i < b; ++i) e += static_cast<double>(out_l[i]) * out_l[i] + static_cast<double>(out_r[i]) * out_r[i];
        return e;
    };
    CHECK(energy(0, 2000) < energy(2000, 20'000)); // до первых отражений тихо
    CHECK(energy(2000, 30'000) > 0.0);
    CHECK(energy(60'000, 96'000) < energy(2000, 36'000) * 0.1); // хвост затухает
    for (std::size_t i = 0; i < n; ++i) { CHECK(std::isfinite(out_l[i])); CHECK(std::isfinite(out_r[i])); }
    CHECK(out_l != out_r); // стерео-разброс

    reverb.clear();
    reverb.process(zero.data(), zero.data(), out_l.data(), out_r.data(), 1000);
    CHECK(energy(0, 1000) == 0.0);
}

TEST_CASE("Reverb масштабируется на другие частоты без выхода за буферы") {
    for (std::uint32_t rate : {8'000u, 22'050u, 44'100u, 96'000u, 192'000u}) {
        Reverb reverb(rate);
        reverb.set({.room_size = 1.0f, .damping = 0.0f, .wet = 1.0f, .width = 0.5f});
        std::vector<float> in(4096, 0.5f), out_l(4096), out_r(4096);
        reverb.process(in.data(), in.data(), out_l.data(), out_r.data(), 4096);
        CHECK(std::isfinite(out_l[4095]));
    }
}

TEST_CASE("Limiter: пики срезаются до потолка, тихое не трогает") {
    Limiter limiter(48'000, 0.9f);
    float l = 0.5f, r = -0.4f;
    CHECK_FALSE(limiter.process(l, r));
    CHECK(l == 0.5f);
    CHECK(r == -0.4f);

    float peak = 0.0f;
    for (int i = 0; i < 10'000; ++i) {
        float a = (i % 2 ? 4.0f : -4.0f), b = 3.0f;
        limiter.process(a, b);
        peak = std::max({peak, std::abs(a), std::abs(b)});
    }
    CHECK(peak <= 0.9f + 1e-6f);
}

} // TEST_SUITE
