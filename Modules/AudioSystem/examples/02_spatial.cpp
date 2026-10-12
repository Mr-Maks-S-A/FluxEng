// 3D-звук: машина проезжает мимо слушателя — панорама уходит слева направо, громкость растёт и падает,
// высота тона скачет по Доплеру. Расчёт открыт игре: compute_spatial можно звать без движка.
#include <AudioSystem/AudioSystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <format>

using namespace AudioSystem;

namespace {
// std::println появится в стандартной библиотеке не у всех компиляторов; std::format есть везде, где собирается движок.
template <class... Args>
void println(std::format_string<Args...> format, Args&&... args) {
    std::puts(std::format(format, std::forward<Args>(args)...).c_str());
}
} // namespace

int main() {
    Listener listener; // в начале координат, смотрит в −Z, право — +X
    Spatial car;
    car.enabled = true;
    car.min_distance = 2.0f;
    car.max_distance = 100.0f;
    car.velocity = {30.0f, 0.0f, 0.0f}; // 108 км/ч слева направо
    car.air_absorption = 0.01f;

    println("{:>7} {:>7} {:>7} {:>7} {:>9}", "x, м", "пан", "гром.", "тон", "срез, Гц");
    float first_pan = 0.0f, last_pan = 0.0f, first_pitch = 0.0f, last_pitch = 0.0f, max_gain = 0.0f;
    for (float x = -40.0f; x <= 40.0f; x += 10.0f) {
        car.position = {x, 0.0f, -5.0f};
        const SpatialResult r = compute_spatial(car, listener);
        println("{:7.1f} {:7.2f} {:7.3f} {:7.3f} {:9.0f}", static_cast<double>(x), static_cast<double>(r.pan), static_cast<double>(r.gain),
                     static_cast<double>(r.pitch), static_cast<double>(r.cutoff_hz));
        if (x == -40.0f) { first_pan = r.pan; first_pitch = r.pitch; }
        last_pan = r.pan;
        last_pitch = r.pitch;
        max_gain = std::max(max_gain, r.gain);
    }

    // То же в движке: источник едет, игра обновляет позицию каждый «кадр».
    AudioEngine engine;
    const VoiceId voice = engine.play(engine.add_clip("engine", make_noise(1.0, 48'000, 1, 0.5f)), {.loop = true, .spatial = car});
    float left_start = 0.0f, right_end = 0.0f;
    for (int frame = 0; frame < 200; ++frame) {
        car.position = {-40.0f + 0.4f * static_cast<float>(frame), 0.0f, -5.0f};
        engine.set_position(voice, car.position, car.velocity);
        std::vector<float> block(960 * 2);
        engine.render(block);
        float l = 0.0f, r = 0.0f;
        for (std::size_t i = 0; i < 960; ++i) { l = std::max(l, std::abs(block[2 * i])); r = std::max(r, std::abs(block[2 * i + 1])); }
        if (frame == 20) left_start = l - r;
        if (frame == 190) right_end = r - l;
    }
    println("в начале слышнее левое ухо ({:+.3f}), в конце правое ({:+.3f})", static_cast<double>(left_start), static_cast<double>(right_end));

    const bool ok = first_pan < -0.9f && last_pan > 0.9f && first_pitch > 1.0f && last_pitch < 1.0f && max_gain > 0.0f && left_start > 0.0f && right_end > 0.0f;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
