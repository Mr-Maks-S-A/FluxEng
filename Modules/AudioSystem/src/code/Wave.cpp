#include <AudioSystem/Wave.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>

namespace AudioSystem {

namespace {

float clamp1(float v) noexcept { return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; }

std::uint32_t next_random(std::uint32_t& state) noexcept {
    state = state * 1664525u + 1013904223u;
    return state;
}

float noise_sample(std::uint32_t& state) noexcept { return static_cast<float>(next_random(state) >> 8) / 8388608.0f - 1.0f; }

std::uint16_t rd16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) noexcept { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }

void wr16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void wr32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void wr_tag(std::vector<std::uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

} // namespace

Wave make_tone(Shape shape, float frequency_hz, float seconds, int sample_rate, float amplitude) {
    Wave wave;
    wave.channels = 1;
    wave.sample_rate = sample_rate;
    const auto frames = static_cast<std::size_t>(std::max(0.0f, seconds) * static_cast<float>(sample_rate));
    wave.samples.resize(frames);
    const std::size_t fade = std::min<std::size_t>(frames / 2, static_cast<std::size_t>(sample_rate / 200)); // 5 мс
    double phase = 0.0;
    const double step = static_cast<double>(frequency_hz) / sample_rate;
    std::uint32_t rng = 12345;
    for (std::size_t i = 0; i < frames; ++i) {
        float v = 0.0f;
        switch (shape) {
            case Shape::Sine: v = static_cast<float>(std::sin(2.0 * std::numbers::pi * phase)); break;
            case Shape::Square: v = phase < 0.5 ? 1.0f : -1.0f; break;
            case Shape::Saw: v = static_cast<float>(2.0 * phase - 1.0); break;
            case Shape::Triangle: v = static_cast<float>(4.0 * std::fabs(phase - 0.5) - 1.0); break;
            case Shape::Noise: v = noise_sample(rng); break;
        }
        float gain = amplitude;
        if (fade > 0 && i + fade > frames) gain *= static_cast<float>(frames - i) / static_cast<float>(fade);
        wave.samples[i] = v * gain;
        phase += step;
        phase -= std::floor(phase);
    }
    return wave;
}

Wave make_noise_burst(float seconds, float decay_per_second, int sample_rate, float amplitude, std::uint32_t seed) {
    Wave wave;
    wave.channels = 1;
    wave.sample_rate = sample_rate;
    const auto frames = static_cast<std::size_t>(std::max(0.0f, seconds) * static_cast<float>(sample_rate));
    wave.samples.resize(frames);
    std::uint32_t rng = seed;
    for (std::size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        wave.samples[i] = noise_sample(rng) * amplitude * std::exp(-decay_per_second * t);
    }
    return wave;
}

std::expected<Wave, std::string> decode_wav(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return std::unexpected(std::string("not a RIFF/WAVE file"));
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    const std::uint8_t* data = nullptr;
    std::size_t data_size = 0;
    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::uint8_t* chunk = bytes.data() + pos;
        const std::size_t size = rd32(chunk + 4);
        const std::size_t body = pos + 8;
        if (body + size > bytes.size()) {
            if (std::memcmp(chunk, "data", 4) == 0) { // оборванный файл: берём, сколько есть
                data = bytes.data() + body;
                data_size = bytes.size() - body;
            }
            break;
        }
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            format = rd16(chunk + 8);
            channels = rd16(chunk + 10);
            rate = rd32(chunk + 12);
            bits = rd16(chunk + 22);
            if (format == 0xFFFE && size >= 26) format = rd16(chunk + 8 + 24); // WAVE_FORMAT_EXTENSIBLE: подформат
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data = bytes.data() + body;
            data_size = size;
        }
        pos = body + size + (size & 1);
    }
    if (channels == 0 || rate == 0) return std::unexpected(std::string("missing fmt chunk"));
    if (data == nullptr) return std::unexpected(std::string("missing data chunk"));
    if (channels > 2) return std::unexpected(std::string("only mono and stereo are supported"));
    const bool is_float = format == 3;
    if (!(format == 1 || is_float)) return std::unexpected("unsupported WAV format " + std::to_string(format));
    if (is_float ? bits != 32 : !(bits == 8 || bits == 16 || bits == 24 || bits == 32))
        return std::unexpected("unsupported bit depth " + std::to_string(bits));

    Wave wave;
    wave.channels = channels;
    wave.sample_rate = static_cast<int>(rate);
    const std::size_t bytes_per = bits / 8u;
    const std::size_t count = data_size / bytes_per / channels * channels;
    wave.samples.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = data + i * bytes_per;
        float v = 0.0f;
        if (is_float) {
            const std::uint32_t raw = rd32(p);
            std::memcpy(&v, &raw, 4);
        } else if (bits == 8) {
            v = (static_cast<float>(p[0]) - 128.0f) / 128.0f;
        } else if (bits == 16) {
            v = static_cast<float>(static_cast<std::int16_t>(rd16(p))) / 32768.0f;
        } else if (bits == 24) {
            std::int32_t s = p[0] | (p[1] << 8) | (p[2] << 16);
            if ((s & 0x800000) != 0) s -= 0x1000000;
            v = static_cast<float>(s) / 8388608.0f;
        } else {
            v = static_cast<float>(static_cast<std::int32_t>(rd32(p))) / 2147483648.0f;
        }
        wave.samples[i] = v;
    }
    return wave;
}

std::vector<std::uint8_t> encode_wav(const Wave& wave) {
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(wave.samples.size() * 2);
    std::vector<std::uint8_t> out;
    out.reserve(44 + data_bytes);
    wr_tag(out, "RIFF");
    wr32(out, 36 + data_bytes);
    wr_tag(out, "WAVE");
    wr_tag(out, "fmt ");
    wr32(out, 16);
    wr16(out, 1); // PCM
    wr16(out, static_cast<std::uint16_t>(wave.channels));
    wr32(out, static_cast<std::uint32_t>(wave.sample_rate));
    wr32(out, static_cast<std::uint32_t>(wave.sample_rate * wave.channels * 2));
    wr16(out, static_cast<std::uint16_t>(wave.channels * 2));
    wr16(out, 16);
    wr_tag(out, "data");
    wr32(out, data_bytes);
    for (const float s : wave.samples) wr16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(clamp1(s) * 32767.0f))));
    return out;
}

std::expected<Wave, std::string> load_wav(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::unexpected("cannot open " + path.string());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return decode_wav(bytes);
}

std::expected<void, std::string> save_wav(const std::filesystem::path& path, const Wave& wave) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return std::unexpected("cannot create " + path.string());
    const std::vector<std::uint8_t> bytes = encode_wav(wave);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) return std::unexpected("write failed: " + path.string());
    return {};
}

} // namespace AudioSystem
