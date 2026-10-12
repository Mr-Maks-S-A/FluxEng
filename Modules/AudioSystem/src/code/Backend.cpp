#include <AudioSystem/Backend.hpp>

#include <algorithm>

namespace AudioSystem {

const std::vector<float>& NullBackend::pump(std::size_t frames) {
    m_buffer.assign(frames * 2, 0.0f);
    if (m_engine) m_engine->render(m_buffer);
    return m_buffer;
}

std::vector<float> render_offline(AudioEngine& engine, double seconds) {
    const auto frames = static_cast<std::size_t>(std::max(0.0, seconds) * engine.sample_rate());
    std::vector<float> out(frames * 2, 0.0f);
    engine.render(out);
    return out;
}

AudioClip render_to_clip(AudioEngine& engine, double seconds) {
    AudioClip clip;
    clip.sample_rate = engine.sample_rate();
    clip.channels = 2;
    clip.samples = render_offline(engine, seconds);
    return clip;
}

} // namespace AudioSystem
