#include <AudioSystem/Dsp.hpp>
#include <AudioSystem/Spatial.hpp>

#include <cmath>
#include <numbers>

namespace AudioSystem {

// ------------------------------------------------------------------------------------------------ LowPass

void LowPass::set_cutoff(float hz, std::uint32_t sample_rate) noexcept {
    const float nyquist = 0.49f * static_cast<float>(sample_rate);
    const float fc = std::clamp(hz, 20.0f, nyquist);
    const float w0 = 2.0f * std::numbers::pi_v<float> * fc / static_cast<float>(sample_rate);
    const float cosw = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * 0.70710678f);
    const float a0 = 1.0f + alpha;
    m_b0 = (1.0f - cosw) * 0.5f / a0;
    m_b1 = (1.0f - cosw) / a0;
    m_b2 = m_b0;
    m_a1 = -2.0f * cosw / a0;
    m_a2 = (1.0f - alpha) / a0;
}

// ------------------------------------------------------------------------------------------------ Reverb

namespace {
// Длины линий задержки Freeverb для 44.1 кГц; для других частот масштабируются.
constexpr std::array<std::size_t, 8> comb_tuning{1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<std::size_t, 4> allpass_tuning{556, 441, 341, 225};
constexpr std::size_t stereo_spread = 23;
constexpr float input_gain = 0.015f;
constexpr float wet_scale = 3.0f;
constexpr float allpass_feedback = 0.5f;

float flush_denormal(float v) noexcept { return std::abs(v) < 1e-20f ? 0.0f : v; }
} // namespace

Reverb::Reverb(std::uint32_t sample_rate) {
    const double scale = static_cast<double>(sample_rate) / 44'100.0;
    for (std::size_t c = 0; c < 2; ++c) {
        const std::size_t spread = c == 0 ? 0 : stereo_spread;
        for (std::size_t i = 0; i < comb_count; ++i) {
            const auto size = std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(comb_tuning[i] + spread) * scale));
            m_channels[c].combs[i].buffer.assign(size, 0.0f);
        }
        for (std::size_t i = 0; i < allpass_count; ++i) {
            const auto size = std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(allpass_tuning[i] + spread) * scale));
            m_channels[c].allpasses[i].buffer.assign(size, 0.0f);
        }
    }
    set(ReverbParams{});
}

void Reverb::set(const ReverbParams& params) noexcept {
    const float room = std::clamp(params.room_size, 0.0f, 1.0f);
    const float damping = std::clamp(params.damping, 0.0f, 1.0f);
    const float width = std::clamp(params.width, 0.0f, 1.0f);
    m_wet = std::clamp(params.wet, 0.0f, 1.0f);
    m_feedback = room * 0.28f + 0.7f;
    m_damp = damping * 0.4f;
    m_wet1 = m_wet * wet_scale * (width * 0.5f + 0.5f);
    m_wet2 = m_wet * wet_scale * ((1.0f - width) * 0.5f);
}

void Reverb::clear() noexcept {
    for (Channel& channel : m_channels) {
        for (Comb& comb : channel.combs) { std::fill(comb.buffer.begin(), comb.buffer.end(), 0.0f); comb.store = 0.0f; comb.index = 0; }
        for (Allpass& ap : channel.allpasses) { std::fill(ap.buffer.begin(), ap.buffer.end(), 0.0f); ap.index = 0; }
    }
}

float Reverb::run(Channel& channel, float input) noexcept {
    float sum = 0.0f;
    for (Comb& comb : channel.combs) {
        const float out = comb.buffer[comb.index];
        comb.store = flush_denormal(out * (1.0f - m_damp) + comb.store * m_damp);
        comb.buffer[comb.index] = input + comb.store * m_feedback;
        if (++comb.index >= comb.buffer.size()) comb.index = 0;
        sum += out;
    }
    for (Allpass& ap : channel.allpasses) {
        const float buffered = ap.buffer[ap.index];
        const float out = -sum + buffered;
        ap.buffer[ap.index] = flush_denormal(sum + buffered * allpass_feedback);
        if (++ap.index >= ap.buffer.size()) ap.index = 0;
        sum = out;
    }
    return sum;
}

void Reverb::process(const float* in_left, const float* in_right, float* out_left, float* out_right, std::uint32_t frames) noexcept {
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float input = (in_left[i] + in_right[i]) * input_gain;
        const float l = run(m_channels[0], input);
        const float r = run(m_channels[1], input);
        out_left[i] = l * m_wet1 + r * m_wet2;
        out_right[i] = r * m_wet1 + l * m_wet2;
    }
}

// ------------------------------------------------------------------------------------------------ Limiter

Limiter::Limiter(std::uint32_t sample_rate, float ceiling, float release_seconds) noexcept
    : m_ceiling(ceiling), m_release(std::exp(-1.0f / (std::max(release_seconds, 1e-4f) * static_cast<float>(sample_rate)))) {}

bool Limiter::process(float& left, float& right) noexcept {
    const float peak = std::max(std::abs(left), std::abs(right));
    m_envelope = std::max(peak, m_envelope * m_release);
    if (m_envelope <= m_ceiling) return false;
    const float gain = m_ceiling / m_envelope;
    left *= gain;
    right *= gain;
    return true;
}

// ------------------------------------------------------------------------------------------------ pan

PanGains pan_constant_power(float pan) noexcept {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    const float angle = (p + 1.0f) * (std::numbers::pi_v<float> / 4.0f);
    return {std::cos(angle), std::sin(angle)};
}

// ------------------------------------------------------------------------------------------------ Spatial

float attenuation(AttenuationModel model, float distance, float min_distance, float max_distance, float rolloff) noexcept {
    const float min_d = std::max(min_distance, 1e-3f);
    const float max_d = std::max(max_distance, min_d);
    const float d = std::clamp(distance, min_d, max_d);
    switch (model) {
    case AttenuationModel::None: return 1.0f;
    case AttenuationModel::Inverse: return min_d / (min_d + std::max(rolloff, 0.0f) * (d - min_d));
    case AttenuationModel::Linear:
        if (max_d <= min_d) return 1.0f;
        return std::clamp(1.0f - std::max(rolloff, 0.0f) * (d - min_d) / (max_d - min_d), 0.0f, 1.0f);
    case AttenuationModel::Exponential: return std::pow(d / min_d, -std::max(rolloff, 0.0f));
    }
    return 1.0f;
}

SpatialResult compute_spatial(const Spatial& s, const Listener& l) noexcept {
    SpatialResult r;
    const Vec3 to_source = s.position - l.position;
    const float distance = length(to_source);
    r.distance = distance;

    r.gain = attenuation(s.model, distance, s.min_distance, s.max_distance, s.rolloff);

    // Направление на источник относительно слушателя -> пан. Вплотную к голове панорама сходится в центр.
    const Vec3 forward = normalized(l.forward);
    const Vec3 right = normalized(cross(forward, l.up), {1.0f, 0.0f, 0.0f});
    if (distance > 1e-5f) {
        const Vec3 dir = to_source * (1.0f / distance);
        const float closeness = std::min(1.0f, distance / std::max(s.min_distance, 1e-3f));
        r.pan = std::clamp(dot(dir, right), -1.0f, 1.0f) * closeness;
    }

    // Конус: угол между направлением источника и линией на слушателя.
    if (s.cone_inner_degrees < 360.0f || s.cone_outer_degrees < 360.0f) {
        if (distance > 1e-5f) {
            const Vec3 emit_dir = normalized(s.direction);
            const Vec3 to_listener = to_source * (-1.0f / distance); // от источника к слушателю
            const float cos_angle = std::clamp(dot(emit_dir, to_listener), -1.0f, 1.0f);
            const float angle = std::acos(cos_angle) * (180.0f / std::numbers::pi_v<float>);
            const float inner = 0.5f * s.cone_inner_degrees;
            const float outer = std::max(0.5f * s.cone_outer_degrees, inner);
            if (angle > inner) {
                const float t = outer > inner ? std::min(1.0f, (angle - inner) / (outer - inner)) : 1.0f;
                r.gain *= 1.0f + (s.cone_outer_gain - 1.0f) * t;
            }
        }
    }

    // Доплер: f' = f·(c + v_слушателя_к_источнику)/(c + v_источника_от_слушателя).
    if (s.doppler > 0.0f && distance > 1e-5f) {
        const Vec3 u = to_source * (1.0f / distance);
        const float limit = 0.9f * speed_of_sound;
        const float vl = std::clamp(dot(l.velocity, u) * s.doppler, -limit, limit);
        const float vs = std::clamp(dot(s.velocity, u) * s.doppler, -limit, limit);
        r.pitch = std::clamp((speed_of_sound + vl) / (speed_of_sound + vs), 0.5f, 2.0f);
    }

    // Воздух и препятствия съедают высокие.
    float cutoff = 20000.0f;
    if (s.air_absorption > 0.0f) {
        cutoff = 20000.0f / (1.0f + s.air_absorption * std::max(0.0f, distance - s.min_distance));
    }
    const float occlusion = std::clamp(s.occlusion, 0.0f, 1.0f);
    if (occlusion > 0.0f) {
        cutoff = std::min(cutoff, 20000.0f + (800.0f - 20000.0f) * occlusion);
        r.gain *= 1.0f - 0.5f * occlusion;
    }
    r.cutoff_hz = std::clamp(cutoff, 800.0f, 20000.0f);
    return r;
}

} // namespace AudioSystem
