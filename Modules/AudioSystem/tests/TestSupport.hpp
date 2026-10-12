#pragma once
// Общие помощники тестов AudioSystem.
#include <AudioSystem/AudioSystem.hpp>

#include <cmath>
#include <vector>

namespace test {

inline std::vector<float> render(AudioSystem::AudioEngine& engine, std::size_t frames) {
    std::vector<float> out(frames * 2, 0.0f);
    engine.render(out);
    return out;
}

inline float left(const std::vector<float>& buf, std::size_t frame) { return buf[frame * 2]; }
inline float right(const std::vector<float>& buf, std::size_t frame) { return buf[frame * 2 + 1]; }

inline float peak(const std::vector<float>& buf, std::size_t from_frame = 0, std::size_t to_frame = static_cast<std::size_t>(-1)) {
    float p = 0.0f;
    const std::size_t end = std::min(buf.size() / 2, to_frame);
    for (std::size_t i = from_frame; i < end; ++i) p = std::max({p, std::abs(left(buf, i)), std::abs(right(buf, i))});
    return p;
}

inline float rms(const std::vector<float>& buf, std::size_t from_frame, std::size_t to_frame) {
    double sum = 0.0;
    for (std::size_t i = from_frame; i < to_frame; ++i) sum += static_cast<double>(left(buf, i)) * left(buf, i) + static_cast<double>(right(buf, i)) * right(buf, i);
    return static_cast<float>(std::sqrt(sum / (2.0 * static_cast<double>(to_frame - from_frame))));
}

inline AudioSystem::EngineConfig small_config() {
    AudioSystem::EngineConfig c;
    c.sample_rate = 48'000;
    c.ducking.clear(); // по умолчанию голос приглушает музыку; тестам это мешает
    return c;
}

} // namespace test
