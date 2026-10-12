// Микшер: клипы, шины, ducking голоса над музыкой, реверберация, офлайн-рендер и сохранение в WAV.
// Запуск: 01_mix_and_buses [файл.wav] — с аргументом результат сохраняется, чтобы его можно было послушать.
#include <AudioSystem/AudioSystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <format>

using namespace AudioSystem;

namespace {
// std::println появится в стандартной библиотеке не у всех компиляторов; std::format есть везде, где собирается движок.
template <class... Args>
void println(std::format_string<Args...> format, Args&&... args) {
    std::puts(std::format(format, std::forward<Args>(args)...).c_str());
}
} // namespace

int main(int argc, char** argv) {
    AudioEngine engine; // 48 кГц, 64 голоса, шины master/sfx/music/voice/ambient/ui, голос приглушает музыку на 9 дБ

    // Клипы создаются кодом; в игре их читают из пака (decode_audio) и регистрируют под именами.
    const ClipId music = engine.add_clip("music", make_sine(220.0f, 4.0, 48'000, 0.4f));
    const ClipId voice = engine.add_clip("voice", make_noise(1.0, 48'000, 3, 0.35f));
    const ClipId click = engine.add_clip("click", make_noise(0.03, 48'000, 7, 0.8f));

    engine.set_reverb({.room_size = 0.75f, .damping = 0.4f, .wet = 0.25f});
    engine.play(music, {.bus = Buses::music, .volume = 0.8f, .loop = true, .fade_in = 0.5f});

    std::vector<float> mix;
    const auto run = [&](double seconds) {
        const auto part = render_offline(engine, seconds);
        mix.insert(mix.end(), part.begin(), part.end());
    };

    run(1.0);
    const float music_alone = engine.bus_meter(Buses::music).peak;

    engine.play(voice, {.bus = Buses::voice, .priority = 255});         // говорит диктор: музыка приседает
    engine.play(click, {.volume = 0.9f, .pan = -0.6f, .reverb_send = 0.8f}); // щелчок в «комнате»
    run(0.8);
    const float music_ducked = engine.bus_meter(Buses::music).peak;

    run(1.5); // диктор замолчал, музыка возвращается
    const float music_back = engine.bus_meter(Buses::music).peak;

    println("музыка одна:        пик {:.3f}", static_cast<double>(music_alone));
    println("во время голоса:    пик {:.3f} ({:.1f} дБ)", static_cast<double>(music_ducked), static_cast<double>(gain_to_db(music_ducked / music_alone)));
    println("после:              пик {:.3f}", static_cast<double>(music_back));
    const Stats stats = engine.stats();
    println("кадров: {}, лимитер сработал в {} блоках, потеряно команд: {}", stats.frames_rendered, stats.limiter_blocks, stats.dropped_commands);

    if (argc > 1) {
        AudioClip out;
        out.sample_rate = engine.sample_rate();
        out.channels = 2;
        out.samples = mix;
        const auto wav = encode_wav(out);
        std::ofstream(argv[1], std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        println("записано: {} ({} байт)", argv[1], wav.size());
    }

    const bool ducked = music_ducked < music_alone * 0.5f;
    const bool recovered = music_back > music_alone * 0.9f;
    return ducked && recovered ? EXIT_SUCCESS : EXIT_FAILURE;
}
