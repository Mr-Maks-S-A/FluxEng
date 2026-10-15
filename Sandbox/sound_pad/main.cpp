/**
 * @file main.cpp
 * @brief SoundPad — простой проект со звуком: клавиатура-пианино на AudioSystem (SDL3) + окно WindowSystem (SDL3).
 *
 * Что показывает:
 * - AudioSystem::Device — вывод звука через SDL3; Mixer — голоса-синтезаторы (ADSR, вибрато) и сэмплы (ударные);
 * - тот же Mixer без устройства: `--wav файл.wav` рендерит мелодию в файл за долю секунды (детерминированно, без звуковой карты);
 * - ввод WindowSystem в собственных кодах (Key, Transition): нажатие — нота, отпускание — затухание;
 * - Core::Game: окно, тик, рисование клавиш.
 *
 * Управление: клавиши A S D F G H J K L ; — белые (до, ре, ми …), W E T Y U O P — чёрные; Z / X — октава ниже / выше;
 * 1–4 — форма волны (синус, квадрат, пила, треугольник); V — вибрато; Пробел — бочка, Enter — хэт; мышь — тоже играет;
 * Esc — выход.
 *
 * Запуск:
 *   SoundPad                      играть (окно + звук)
 *   SoundPad --autoplay [--ticks N]   «Ода к радости» играет сама (для проверки); в конце «SoundPad: OK»
 *   SoundPad --wav out.wav        без окна и устройства: отрендерить мелодию в WAV; в конце «SoundPad --wav: OK»
 *   SoundPad --mute               окно без звукового устройства
 */

#include <AudioSystem/AudioSystem.hpp>
#include <Core/Core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <vector>

using InputSystem::Key;
using InputSystem::Transition;
using namespace RendererSystem;

namespace {

namespace as = AudioSystem;

// ------------------------------------------------------------------------------------------------ мелодия

struct Note {
    int midi;
    float start_beat;
    float beats;
};

constexpr float kTempoBpm = 120.0f;

/// «Ода к радости» (Бетховен): первые четыре строки.
std::vector<Note> ode_to_joy() {
    // ми ми фа соль | соль фа ми ре | до до ре ми | ми(.) ре(~) ре(-)
    constexpr int phrases[][2] = {
        {64, 1}, {64, 1}, {65, 1}, {67, 1}, {67, 1}, {65, 1}, {64, 1}, {62, 1}, {60, 1}, {60, 1}, {62, 1}, {64, 1}, {64, 2}, {62, 1}, {62, 2},
        {64, 1}, {64, 1}, {65, 1}, {67, 1}, {67, 1}, {65, 1}, {64, 1}, {62, 1}, {60, 1}, {60, 1}, {62, 1}, {64, 1}, {62, 2}, {60, 1}, {60, 2},
    };
    std::vector<Note> out;
    float beat = 0.0f;
    for (const auto& p : phrases) {
        out.push_back({p[0], beat, static_cast<float>(p[1]) * 0.9f});
        beat += static_cast<float>(p[1]);
    }
    return out;
}

float song_beats(const std::vector<Note>& song) {
    float end = 0.0f;
    for (const Note& n : song) end = std::max(end, n.start_beat + n.beats);
    return end;
}

as::ToneParams voice_for(int midi, as::Shape shape, bool vibrato) {
    as::ToneParams p;
    p.shape = shape;
    p.frequency = as::midi_to_hz(static_cast<float>(midi));
    p.volume = shape == as::Shape::Sine || shape == as::Shape::Triangle ? 0.35f : 0.18f; // квадрат и пила громче по тембру
    p.pan = std::clamp((static_cast<float>(midi) - 66.0f) / 24.0f, -0.6f, 0.6f);        // выше нота — правее
    p.envelope = {.attack = 0.008f, .decay = 0.12f, .sustain = 0.55f, .release = 0.25f};
    if (vibrato) {
        p.vibrato_hz = 5.5f;
        p.vibrato_depth = 0.25f;
    }
    return p;
}

// ------------------------------------------------------------------------------------------------ ударные (сэмплы)

std::shared_ptr<const as::Wave> make_kick() {
    auto wave = std::make_shared<as::Wave>();
    wave->sample_rate = 48000;
    const std::size_t frames = 12000; // 0,25 с
    wave->samples.resize(frames);
    double phase = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        const double hz = 45.0 + 120.0 * std::exp(-t * 30.0); // быстрый спад частоты — «бум»
        phase += hz / 48000.0;
        wave->samples[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * phase) * std::exp(-t * 11.0) * 0.9);
    }
    return wave;
}

std::shared_ptr<const as::Wave> make_hat() { return std::make_shared<as::Wave>(as::make_noise_burst(0.09f, 40.0f, 48000, 0.45f, 3)); }

// ------------------------------------------------------------------------------------------------ рендер в WAV (без окна и устройства)

int render_wav(const std::string& path) {
    const std::vector<Note> song = ode_to_joy();
    const int rate = 48000;
    as::Mixer mixer(rate);
    const float seconds_per_beat = 60.0f / kTempoBpm;
    const std::size_t total_frames = static_cast<std::size_t>((song_beats(song) * seconds_per_beat + 0.6f) * static_cast<float>(rate));

    as::Wave wave;
    wave.channels = 2;
    wave.sample_rate = rate;
    wave.samples.resize(total_frames * 2);

    const auto kick = make_kick();
    std::size_t next = 0;
    std::vector<Note> sorted = song;
    std::sort(sorted.begin(), sorted.end(), [](const Note& a, const Note& b) { return a.start_beat < b.start_beat; });
    constexpr std::size_t block = 256;
    for (std::size_t frame = 0; frame < total_frames; frame += block) {
        const std::size_t n = std::min(block, total_frames - frame);
        const float now = static_cast<float>(frame) / static_cast<float>(rate);
        while (next < sorted.size() && sorted[next].start_beat * seconds_per_beat <= now) {
            mixer.play_note(voice_for(sorted[next].midi, as::Shape::Triangle, false), sorted[next].beats * seconds_per_beat);
            ++next;
        }
        // бочка на каждую сильную долю
        if (std::fmod(now, seconds_per_beat * 2.0f) < static_cast<float>(block) / static_cast<float>(rate)) mixer.play(kick, {.volume = 0.6f});
        mixer.render(std::span<float>(wave.samples.data() + frame * 2, n * 2));
    }

    float peak = 0.0f;
    double sum = 0.0;
    bool finite = true;
    for (float s : wave.samples) {
        peak = std::max(peak, std::fabs(s));
        sum += static_cast<double>(s) * s;
        finite = finite && std::isfinite(s);
    }
    const double rms = std::sqrt(sum / static_cast<double>(wave.samples.size()));
    if (const auto saved = as::save_wav(path, wave); !saved) {
        std::println(stderr, "SoundPad --wav: {}", saved.error());
        return 1;
    }
    const auto back = as::load_wav(path);
    const bool round_trip = back && back->frames() == wave.frames() && back->channels == 2;
    std::println("SoundPad --wav: {} нот, {:.2f} с, пик {:.3f}, rms {:.3f} → {}", sorted.size(), static_cast<double>(total_frames) / rate, peak, rms, path);
    const bool ok = finite && peak > 0.05f && peak <= 1.0f && rms > 0.01 && round_trip && next == sorted.size();
    std::println("{}", ok ? "SoundPad --wav: OK" : "SoundPad --wav: ОШИБКА");
    return ok ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------ игра

struct PianoKey {
    int semitone;     ///< от до выбранной октавы
    Key key;
    bool black;
    float x, y, w, h; ///< в координатах мира
};

constexpr float kWorldW = 1060.0f;
constexpr float kWorldH = 440.0f;

std::vector<PianoKey> build_keys() {
    constexpr std::array<Key, 10> white_keys = {Key::A, Key::S, Key::D, Key::F, Key::G, Key::H, Key::J, Key::K, Key::L, Key::Semicolon};
    constexpr std::array<int, 10> white_semi = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16};
    constexpr float key_w = 96.0f, x0 = 50.0f, y0 = 70.0f;
    std::vector<PianoKey> keys;
    for (std::size_t i = 0; i < white_keys.size(); ++i)
        keys.push_back({white_semi[i], white_keys[i], false, x0 + static_cast<float>(i) * key_w, y0, key_w - 3.0f, 300.0f});
    struct Black { Key key; std::size_t after_white; };
    constexpr std::array<Black, 7> blacks = {{{Key::W, 0}, {Key::E, 1}, {Key::T, 3}, {Key::Y, 4}, {Key::U, 5}, {Key::O, 7}, {Key::P, 8}}};
    for (const Black& b : blacks)
        keys.push_back({white_semi[b.after_white] + 1, b.key, true, x0 + static_cast<float>(b.after_white + 1) * key_w - 30.0f, y0, 56.0f, 180.0f});
    return keys;
}

class SoundPad final : public Core::Game {
public:
    [[nodiscard]] glm::vec2 world_size() const override { return {kWorldW, kWorldH}; }

    void setup(Core::App& app) override {
        const auto& args = app.config().extra_args;
        autoplay = std::find(args.begin(), args.end(), "--autoplay") != args.end();
        const bool mute = std::find(args.begin(), args.end(), "--mute") != args.end();
        if (!mute) {
            if (auto created = as::Device::create({.sample_rate = 48000, .buffer_frames = 512})) {
                device.emplace(std::move(*created));
                audio_info = "audio: " + device->driver_name();
            } else {
                audio_info = "audio: нет устройства (" + created.error() + ")";
                std::println(stderr, "SoundPad: {}", audio_info);
            }
        } else {
            audio_info = "audio: выключен (--mute)";
        }
        kick = make_kick();
        hat = make_hat();
        keys = build_keys();
        song = ode_to_joy();
        std::sort(song.begin(), song.end(), [](const Note& a, const Note& b) { return a.start_beat < b.start_beat; });

        app.window().events().key.subscribe([this](Key key, Transition t) { on_key(key, t); });
    }

    void frame(Core::App& app, float seconds) override {
        for (float& g : glow) g = std::max(0.0f, g - seconds * 2.5f);
        // мышь играет клавиши: нажатие берёт ноту, отпускание и уход с клавиши — отпускает
        const bool mouse_down = app.input().down[0];
        const std::optional<std::size_t> under = key_at(app.input().mouse_world);
        if (mouse_down && under && (!mouse_key || *mouse_key != *under)) {
            if (mouse_key) key_up(*mouse_key);
            mouse_key = under;
            key_down(*under);
        } else if (!mouse_down && mouse_key) {
            key_up(*mouse_key);
            mouse_key.reset();
        }
    }

    void tick(Core::App& app) override {
        if (!autoplay) return;
        // 30 тиков в секунду, 120 уд/мин → 15 тиков на долю
        const float beat = static_cast<float>(app.tick()) / (30.0f * 60.0f / kTempoBpm);
        while (auto_next < song.size() && song[auto_next].start_beat <= beat) {
            const Note& n = song[auto_next++];
            const int midi = n.midi;
            glow_for_midi(midi);
            const float hold = n.beats * 60.0f / kTempoBpm;
            if (device) device->play_note(voice_for(midi, shape, vibrato), hold);
            ++notes_played;
        }
        if (auto_next >= song.size() && beat > song_beats(song) + 1.0f) app.window().request_close();
    }

    void render(Core::App& app, Renderer2D& r) override {
        r.fill_rect({{0.0f, 0.0f}, world_size()}, Color::from_rgba(0x14161CFF), -10);
        const FontHandle font = app.ui_font();
        r.draw_text(font, "SoundPad", {50.0f, 16.0f}, {.size = 30.0f, .color = Colors::white});
        r.draw_text(font, std::format("октава {}  |  волна: {}  |  вибрато: {}  |  голосов: {}", octave, shape_name(), vibrato ? "да" : "нет",
                                      device ? device->active_voices() : 0),
                    {50.0f, 380.0f}, {.size = 20.0f, .color = Color::from_rgba(0xB8C0D0FF)});
        r.draw_text(font, "A S D F G H J K L ; · W E T Y U O P · Z/X октава · 1-4 волна · V вибрато · Пробел/Enter ударные", {50.0f, 408.0f},
                    {.size = 16.0f, .color = Color::from_rgba(0x7C869CFF)});
        for (int pass = 0; pass < 2; ++pass) { // белые под чёрными
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const PianoKey& k = keys[i];
                if (k.black != (pass == 1)) continue;
                const float lit = std::min(1.0f, glow[i]);
                const bool down = held[i];
                Color base = k.black ? Color::from_rgba(0x1E2128FF) : Color::from_rgba(0xEDEFF4FF);
                Color hot = Color::from_rgba(down ? 0xFFB347FF : 0xFFD79AFF);
                const Color c = down || lit > 0.01f ? mix(base, hot, down ? 1.0f : lit) : base;
                r.fill_rect({{k.x, k.y}, {k.w, k.h}}, c, pass);
            }
        }
    }

    void shutdown(Core::App& app) override {
        if (device) device->stop_all();
        if (!autoplay) return;
        const bool audio_ran = !device || device->frames_rendered() > 0;
        const bool ok = notes_played == song.size() && audio_ran;
        std::println("SoundPad: {} нот из {}, {}, кадров звука {}", notes_played, song.size(), audio_info, device ? device->frames_rendered() : 0);
        std::println("{}", ok ? "SoundPad: OK" : "SoundPad: ОШИБКА");
        (void)app;
    }

    [[nodiscard]] std::string status() const override { return std::format("SoundPad | октава {} | {} | {}", octave, shape_name(), audio_info); }

private:
    static Color mix(Color a, Color b, float t) {
        const auto lerp = [t](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t); };
        return Color{lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), 255};
    }

    [[nodiscard]] const char* shape_name() const {
        switch (shape) {
            case as::Shape::Sine: return "синус";
            case as::Shape::Square: return "квадрат";
            case as::Shape::Saw: return "пила";
            case as::Shape::Triangle: return "треугольник";
            default: return "шум";
        }
    }

    [[nodiscard]] int midi_of(std::size_t key_index) const { return 12 * (octave + 1) + keys[key_index].semitone; }

    [[nodiscard]] std::optional<std::size_t> key_at(glm::vec2 p) const {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const PianoKey& k = keys[i];
            if (p.x < k.x || p.x > k.x + k.w || p.y < k.y || p.y > k.y + k.h) continue;
            if (!best || (k.black && !keys[*best].black)) best = i; // чёрные поверх белых
        }
        return best;
    }

    void glow_for_midi(int midi) {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (12 * 5 + keys[i].semitone == midi) glow[i] = 1.0f; // автоигра подсвечивает клавиши четвёртой октавы
    }

    void key_down(std::size_t i) {
        if (held[i]) return;
        held[i] = true;
        glow[i] = 1.0f;
        if (device) voices[i] = device->note_on(voice_for(midi_of(i), shape, vibrato));
    }

    void key_up(std::size_t i) {
        if (!held[i]) return;
        held[i] = false;
        if (device) device->release(voices[i]);
    }

    void on_key(Key key, Transition t) {
        if (t == Transition::Repeat) return;
        const bool press = t == Transition::Press;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i].key != key) continue;
            if (press) key_down(i);
            else key_up(i);
            return;
        }
        if (!press) return;
        switch (key) {
            case Key::Z: octave = std::max(1, octave - 1); break;
            case Key::X: octave = std::min(7, octave + 1); break;
            case Key::Num1: shape = as::Shape::Sine; break;
            case Key::Num2: shape = as::Shape::Square; break;
            case Key::Num3: shape = as::Shape::Saw; break;
            case Key::Num4: shape = as::Shape::Triangle; break;
            case Key::V: vibrato = !vibrato; break;
            case Key::Space:
                if (device) device->play(kick, {.volume = 0.8f});
                break;
            case Key::Enter:
                if (device) device->play(hat, {.volume = 0.6f, .pan = 0.3f});
                break;
            default: break;
        }
    }

    std::optional<as::Device> device;
    std::string audio_info = "audio: —";
    std::shared_ptr<const as::Wave> kick, hat;
    std::vector<PianoKey> keys;
    std::array<bool, 32> held{};
    std::array<float, 32> glow{};
    std::array<as::VoiceId, 32> voices{};
    std::optional<std::size_t> mouse_key;
    int octave = 4;
    as::Shape shape = as::Shape::Triangle;
    bool vibrato = false;
    bool autoplay = false;
    std::vector<Note> song;
    std::size_t auto_next = 0;
    std::size_t notes_played = 0;
};

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--wav") == 0) return render_wav(i + 1 < argc ? argv[i + 1] : "sound_pad.wav");
    }
    // Esc закрывает окно; пробел — ударные, поэтому паузу симуляции Core отключаем.
    return Core::run<SoundPad>({.title = "SoundPad", .width = 1060, .height = 440, .ticks_per_second = 30.0, .pause_key = Key::Unknown, .camera_controls = false},
                               argc, argv);
}
