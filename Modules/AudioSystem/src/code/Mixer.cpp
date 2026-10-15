#include <AudioSystem/Mixer.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace AudioSystem {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

float clamp1(float v) noexcept { return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; }

/// Поправка PolyBLEP: сглаживает скачок в точке перехода фазы, чтобы пила и квадрат не звенели алиасингом.
double poly_blep(double t, double dt) noexcept {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

} // namespace

float midi_to_hz(float midi_note) noexcept { return 440.0f * std::exp2((midi_note - 69.0f) / 12.0f); }

struct Mixer::Voice {
    enum class Stage : std::uint8_t { Attack, Decay, Sustain, Release };

    bool active = false;
    bool is_tone = false;
    std::uint32_t id = 0;
    std::uint64_t age = 0;

    // сэмпл
    std::shared_ptr<const Wave> wave;
    double position = 0.0; ///< в кадрах сэмпла
    double step = 1.0;     ///< кадров сэмпла на кадр микшера
    bool loop = false;

    // синтезатор
    Shape shape = Shape::Sine;
    double phase = 0.0;
    double frequency = 440.0;
    double vibrato_phase = 0.0;
    float vibrato_hz = 0.0f;
    float vibrato_depth = 0.0f;
    std::uint32_t rng = 1;

    float volume = 1.0f;
    float gain_left = 0.7071f;
    float gain_right = 0.7071f;

    // огибающая
    Envelope envelope{};
    Stage stage = Stage::Attack;
    float level = 0.0f;
    float release_step = 0.0f;
    std::int64_t hold_left = -1; ///< кадров до автоотпускания; <0 — держать

    void begin_release(int rate) noexcept {
        if (stage == Stage::Release) return;
        stage = Stage::Release;
        const float frames = std::max(1.0f, envelope.release * static_cast<float>(rate));
        release_step = std::max(level, 1e-6f) / frames;
    }
};

Mixer::Mixer(int sample_rate, std::size_t max_voices) : m_rate(sample_rate > 0 ? sample_rate : 48000), m_voices(std::max<std::size_t>(1, max_voices)) {}

Mixer::Mixer(Mixer&&) noexcept = default;
Mixer& Mixer::operator=(Mixer&&) noexcept = default;
Mixer::~Mixer() = default;

VoiceId Mixer::start(Voice&& voice) {
    Voice* slot = nullptr;
    for (Voice& v : m_voices) {
        if (!v.active) {
            slot = &v;
            break;
        }
    }
    if (slot == nullptr) { // все заняты: вытесняем самый старый
        slot = &*std::min_element(m_voices.begin(), m_voices.end(), [](const Voice& a, const Voice& b) { return a.age < b.age; });
    }
    voice.active = true;
    voice.id = m_next_id++;
    if (m_next_id == 0) m_next_id = 1;
    voice.age = m_frames + voice.id; // монотонно растёт
    const VoiceId id{voice.id};
    *slot = std::move(voice);
    return id;
}

namespace {

void set_pan(float pan, float& left, float& right) noexcept {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    const float angle = (p + 1.0f) * static_cast<float>(std::numbers::pi) / 4.0f;
    left = std::cos(angle);
    right = std::sin(angle);
}

} // namespace

VoiceId Mixer::play(std::shared_ptr<const Wave> wave, PlayParams params) {
    if (!wave || wave->frames() == 0 || (wave->channels != 1 && wave->channels != 2)) return {};
    Voice v;
    v.wave = std::move(wave);
    v.step = static_cast<double>(v.wave->sample_rate) / m_rate * std::max(0.01f, params.pitch);
    v.loop = params.loop;
    v.volume = params.volume;
    set_pan(params.pan, v.gain_left, v.gain_right);
    v.envelope = {.attack = 0.001f, .decay = 0.0f, .sustain = 1.0f, .release = 0.015f};
    return start(std::move(v));
}

VoiceId Mixer::note_on(const ToneParams& params) {
    Voice v;
    v.is_tone = true;
    v.shape = params.shape;
    v.frequency = std::max(1.0f, params.frequency);
    v.vibrato_hz = params.vibrato_hz;
    v.vibrato_depth = params.vibrato_depth;
    v.volume = params.volume;
    v.rng = 0x9E3779B9u ^ m_next_id;
    set_pan(params.pan, v.gain_left, v.gain_right);
    v.envelope = params.envelope;
    v.envelope.sustain = std::clamp(v.envelope.sustain, 0.0f, 1.0f);
    return start(std::move(v));
}

VoiceId Mixer::play_note(const ToneParams& params, float hold_seconds) {
    const VoiceId id = note_on(params);
    if (Voice* v = find(id)) v->hold_left = static_cast<std::int64_t>(std::max(0.0f, hold_seconds) * static_cast<float>(m_rate));
    return id;
}

Mixer::Voice* Mixer::find(VoiceId id) noexcept {
    if (!id) return nullptr;
    for (Voice& v : m_voices)
        if (v.active && v.id == id.value) return &v;
    return nullptr;
}

const Mixer::Voice* Mixer::find(VoiceId id) const noexcept { return const_cast<Mixer*>(this)->find(id); }

void Mixer::release(VoiceId id) noexcept {
    if (Voice* v = find(id)) v->begin_release(m_rate);
}

void Mixer::stop(VoiceId id) noexcept {
    if (Voice* v = find(id)) v->active = false;
}

void Mixer::stop_all() noexcept {
    for (Voice& v : m_voices) v.active = false;
}

std::size_t Mixer::active_voices() const noexcept {
    return static_cast<std::size_t>(std::count_if(m_voices.begin(), m_voices.end(), [](const Voice& v) { return v.active; }));
}

bool Mixer::is_active(VoiceId id) const noexcept { return find(id) != nullptr; }

void Mixer::render(std::span<float> stereo) noexcept {
    std::fill(stereo.begin(), stereo.end(), 0.0f);
    const std::size_t frames = stereo.size() / 2;
    const auto rate = static_cast<float>(m_rate);

    for (Voice& v : m_voices) {
        if (!v.active) continue;
        const float attack_step = 1.0f / std::max(1.0f, v.envelope.attack * rate);
        const float decay_step = (1.0f - v.envelope.sustain) / std::max(1.0f, v.envelope.decay * rate);

        for (std::size_t i = 0; i < frames; ++i) {
            // огибающая
            switch (v.stage) {
                case Voice::Stage::Attack:
                    v.level += attack_step;
                    if (v.level >= 1.0f) { v.level = 1.0f; v.stage = Voice::Stage::Decay; }
                    break;
                case Voice::Stage::Decay:
                    v.level -= decay_step;
                    if (v.level <= v.envelope.sustain) { v.level = v.envelope.sustain; v.stage = Voice::Stage::Sustain; }
                    break;
                case Voice::Stage::Sustain:
                    if (v.hold_left >= 0 && v.hold_left-- == 0) v.begin_release(m_rate);
                    break;
                case Voice::Stage::Release:
                    v.level -= v.release_step;
                    if (v.level <= 0.0f) { v.active = false; }
                    break;
            }
            if (!v.active) break;

            float sample_l = 0.0f, sample_r = 0.0f;
            if (v.is_tone) {
                double hz = v.frequency;
                if (v.vibrato_hz > 0.0f && v.vibrato_depth != 0.0f) {
                    hz *= std::exp2(static_cast<double>(v.vibrato_depth) * std::sin(kTwoPi * v.vibrato_phase) / 12.0);
                    v.vibrato_phase += static_cast<double>(v.vibrato_hz) / m_rate;
                    v.vibrato_phase -= std::floor(v.vibrato_phase);
                }
                const double dt = hz / m_rate;
                double s = 0.0;
                switch (v.shape) {
                    case Shape::Sine: s = std::sin(kTwoPi * v.phase); break;
                    case Shape::Square:
                        s = v.phase < 0.5 ? 1.0 : -1.0;
                        s += poly_blep(v.phase, dt);
                        s -= poly_blep(std::fmod(v.phase + 0.5, 1.0), dt);
                        break;
                    case Shape::Saw: s = 2.0 * v.phase - 1.0 - poly_blep(v.phase, dt); break;
                    case Shape::Triangle: s = 4.0 * std::fabs(v.phase - 0.5) - 1.0; break;
                    case Shape::Noise:
                        v.rng = v.rng * 1664525u + 1013904223u;
                        s = static_cast<double>(v.rng >> 8) / 8388608.0 - 1.0;
                        break;
                }
                v.phase += dt;
                v.phase -= std::floor(v.phase);
                sample_l = sample_r = static_cast<float>(s);
            } else {
                const Wave& w = *v.wave;
                const auto frame = static_cast<std::size_t>(v.position);
                if (frame >= w.frames()) { v.active = false; break; }
                const std::size_t next = frame + 1 < w.frames() ? frame + 1 : (v.loop ? 0 : frame);
                const auto frac = static_cast<float>(v.position - static_cast<double>(frame));
                const auto ch = static_cast<std::size_t>(w.channels);
                const auto at = [&](std::size_t f, std::size_t c) { return w.samples[f * ch + (c < ch ? c : 0)]; };
                sample_l = at(frame, 0) + (at(next, 0) - at(frame, 0)) * frac;
                sample_r = ch > 1 ? at(frame, 1) + (at(next, 1) - at(frame, 1)) * frac : sample_l;
                v.position += v.step;
                if (v.loop) {
                    const auto len = static_cast<double>(w.frames());
                    if (v.position >= len) v.position = std::fmod(v.position, len);
                }
            }
            const float gain = v.level * v.volume;
            stereo[i * 2] += sample_l * gain * v.gain_left;
            stereo[i * 2 + 1] += sample_r * gain * v.gain_right;
        }
    }
    for (float& s : stereo) s = clamp1(s * m_master);
    m_frames += frames;
}

} // namespace AudioSystem
