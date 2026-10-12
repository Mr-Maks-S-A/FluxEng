#include <AudioSystem/Clip.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>

namespace AudioSystem {

namespace {

std::uint16_t read_u16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8));
}
std::uint32_t read_u32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(p[0])) | (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[2])) << 16) | (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[3])) << 24);
}

void put_u16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
}
void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}
void put_tag(std::vector<std::byte>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(tag[i]));
}

bool tag_is(const std::byte* p, const char* tag) noexcept { return std::memcmp(p, tag, 4) == 0; }

float read_sample(const std::byte* p, std::uint16_t format, std::uint16_t bits) noexcept {
    if (format == 3) { // float
        if (bits == 32) return std::bit_cast<float>(read_u32(p));
        const std::uint64_t lo = read_u32(p);
        const std::uint64_t hi = read_u32(p + 4);
        return static_cast<float>(std::bit_cast<double>((hi << 32) | lo));
    }
    switch (bits) {
    case 8: return (static_cast<float>(std::to_integer<unsigned>(p[0])) - 128.0f) / 128.0f;
    case 16: return static_cast<float>(static_cast<std::int16_t>(read_u16(p))) / 32768.0f;
    case 24: {
        std::int32_t v = static_cast<std::int32_t>(read_u32(p) << 8); // знак берём из старшего байта
        return static_cast<float>(v >> 8) / 8388608.0f;
    }
    default: return static_cast<float>(static_cast<std::int32_t>(read_u32(p))) / 2147483648.0f;
    }
}

/// Сводит интерливленные данные `src_channels` каналов к 1 или 2: чётные каналы — влево, нечётные — вправо (грубо, но без сюрпризов).
void downmix(std::vector<float>& samples, std::uint16_t& channels) {
    if (channels <= 2) return;
    const std::size_t frames = samples.size() / channels;
    std::vector<float> out(frames * 2);
    const std::size_t left_count = (channels + 1u) / 2u;
    const std::size_t right_count = channels / 2u;
    for (std::size_t f = 0; f < frames; ++f) {
        float l = 0.0f, r = 0.0f;
        for (std::size_t c = 0; c < channels; ++c) {
            const float s = samples[f * channels + c];
            if (c % 2 == 0) l += s; else r += s;
        }
        out[f * 2] = l / static_cast<float>(left_count);
        out[f * 2 + 1] = r / static_cast<float>(right_count);
    }
    samples = std::move(out);
    channels = 2;
}

} // namespace

DecodeResult decode_wav(std::span<const std::byte> bytes, const DecodeLimits& limits) {
    if (bytes.size() < 12 || !tag_is(bytes.data(), "RIFF") || !tag_is(bytes.data() + 8, "WAVE")) {
        return std::unexpected("wav: missing RIFF/WAVE header");
    }

    bool have_fmt = false;
    std::uint16_t format = 0, channels = 0, block_align = 0, bits = 0;
    std::uint32_t rate = 0;
    const std::byte* data = nullptr;
    std::size_t data_size = 0;
    std::uint64_t loop_begin = 0, loop_end = 0;

    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::byte* chunk = bytes.data() + pos;
        const std::size_t declared = read_u32(chunk + 4);
        const std::size_t body = pos + 8;
        const std::size_t available = bytes.size() - body;
        // Размер блока данных часто врёт (потоковые записи пишут 0 или 0xFFFFFFFF): берём то, что есть. Остальные блоки обязаны помещаться.
        const bool is_data = tag_is(chunk, "data");
        if (declared > available && !is_data) break;
        const std::size_t size = std::min(declared, available);

        if (tag_is(chunk, "fmt ")) {
            if (size < 16) return std::unexpected("wav: fmt chunk is too short");
            format = read_u16(bytes.data() + body);
            channels = read_u16(bytes.data() + body + 2);
            rate = read_u32(bytes.data() + body + 4);
            block_align = read_u16(bytes.data() + body + 12);
            bits = read_u16(bytes.data() + body + 14);
            if (format == 0xFFFE) { // WAVE_FORMAT_EXTENSIBLE: настоящий формат — первые два байта GUID
                if (size < 26) return std::unexpected("wav: extensible fmt chunk is too short");
                format = read_u16(bytes.data() + body + 24);
            }
            have_fmt = true;
        } else if (is_data) {
            data = bytes.data() + body;
            data_size = size;
            if (!have_fmt) return std::unexpected("wav: data chunk before fmt chunk");
        } else if (tag_is(chunk, "smpl") && size >= 36 + 24) {
            const std::uint32_t loops = read_u32(bytes.data() + body + 28);
            if (loops >= 1) {
                loop_begin = read_u32(bytes.data() + body + 36 + 8);
                loop_end = static_cast<std::uint64_t>(read_u32(bytes.data() + body + 36 + 12)) + 1; // в smpl конец включительно
            }
        }
        pos = body + size + (size & 1u); // блоки выровнены по 2 байтам
        if (is_data && data) {
            // smpl обычно идёт после data: продолжаем разбор, но data уже найден.
        }
    }

    if (!have_fmt) return std::unexpected("wav: no fmt chunk");
    if (!data) return std::unexpected("wav: no data chunk");
    if (format != 1 && format != 3) return std::unexpected("wav: unsupported format (only PCM and IEEE float)");
    if (channels == 0 || channels > 8) return std::unexpected("wav: unsupported channel count");
    if (rate < 1000 || rate > limits.max_sample_rate) return std::unexpected("wav: sample rate is out of range");
    const bool bits_ok = format == 3 ? (bits == 32 || bits == 64) : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!bits_ok) return std::unexpected("wav: unsupported bit depth");
    const std::size_t bytes_per_sample = bits / 8u;
    if (block_align != bytes_per_sample * channels) return std::unexpected("wav: inconsistent block alignment");

    const std::uint64_t frames = data_size / block_align;
    if (frames == 0) return std::unexpected("wav: no audio frames");
    if (frames > limits.max_frames) return std::unexpected("wav: clip is longer than the allowed limit");

    AudioClip clip;
    clip.sample_rate = rate;
    clip.channels = channels;
    clip.samples.resize(static_cast<std::size_t>(frames) * channels);
    for (std::size_t i = 0; i < clip.samples.size(); ++i) {
        clip.samples[i] = read_sample(data + i * bytes_per_sample, format, bits);
    }
    downmix(clip.samples, clip.channels);
    if (loop_end > loop_begin && loop_end <= frames) {
        clip.loop_begin = loop_begin;
        clip.loop_end = loop_end;
    }
    return clip;
}

DecodeResult decode_audio(std::span<const std::byte> bytes, const DecodeLimits& limits) {
    if (bytes.size() >= 12 && tag_is(bytes.data(), "RIFF") && tag_is(bytes.data() + 8, "WAVE")) return decode_wav(bytes, limits);
    if (bytes.size() >= 4 && tag_is(bytes.data(), "OggS")) return decode_ogg(bytes, limits);
    return std::unexpected("audio: unknown format (expected WAV or OGG)");
}

std::vector<std::byte> encode_wav(const AudioClip& clip, WavFormat format) {
    std::vector<std::byte> out;
    if (!clip.valid()) return out;
    const bool is_float = format == WavFormat::Float32;
    const std::uint16_t bits = is_float ? 32 : 16;
    const std::uint16_t block_align = static_cast<std::uint16_t>(clip.channels * bits / 8u);
    const std::uint32_t data_size = static_cast<std::uint32_t>(clip.samples.size() * (bits / 8u));
    const bool has_loop = clip.loop_end > clip.loop_begin;
    const std::uint32_t smpl_size = has_loop ? 8 + 36 + 24 : 0;

    out.reserve(44 + data_size + smpl_size);
    put_tag(out, "RIFF");
    put_u32(out, 4 + (8 + 16) + (8 + data_size) + smpl_size);
    put_tag(out, "WAVE");
    put_tag(out, "fmt ");
    put_u32(out, 16);
    put_u16(out, is_float ? 3 : 1);
    put_u16(out, clip.channels);
    put_u32(out, clip.sample_rate);
    put_u32(out, clip.sample_rate * block_align);
    put_u16(out, block_align);
    put_u16(out, bits);
    put_tag(out, "data");
    put_u32(out, data_size);
    for (const float s : clip.samples) {
        if (is_float) {
            put_u32(out, std::bit_cast<std::uint32_t>(s));
        } else {
            const float clamped = std::clamp(s, -1.0f, 1.0f);
            put_u16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(clamped * 32767.0f))));
        }
    }
    if (has_loop) {
        put_tag(out, "smpl");
        put_u32(out, 36 + 24);
        for (int i = 0; i < 7; ++i) put_u32(out, 0);
        put_u32(out, 1); // число петель
        put_u32(out, 0); // данные сэмплера
        put_u32(out, 0); // id
        put_u32(out, 0); // тип: прямая
        put_u32(out, static_cast<std::uint32_t>(clip.loop_begin));
        put_u32(out, static_cast<std::uint32_t>(clip.loop_end - 1));
        put_u32(out, 0);
        put_u32(out, 0);
    }
    return out;
}

AudioClip make_sine(float frequency, double seconds, std::uint32_t sample_rate, float amplitude) {
    AudioClip clip;
    clip.sample_rate = sample_rate;
    clip.channels = 1;
    const auto frames = static_cast<std::size_t>(std::max(0.0, seconds) * sample_rate);
    clip.samples.resize(frames);
    const double step = 2.0 * std::numbers::pi * static_cast<double>(frequency) / sample_rate;
    for (std::size_t i = 0; i < frames; ++i) clip.samples[i] = amplitude * static_cast<float>(std::sin(step * static_cast<double>(i)));
    return clip;
}

AudioClip make_constant(float value, std::uint64_t frames, std::uint32_t sample_rate, std::uint16_t channels) {
    AudioClip clip;
    clip.sample_rate = sample_rate;
    clip.channels = channels;
    clip.samples.assign(static_cast<std::size_t>(frames) * channels, value);
    return clip;
}

AudioClip make_noise(double seconds, std::uint32_t sample_rate, std::uint64_t seed, float amplitude) {
    AudioClip clip;
    clip.sample_rate = sample_rate;
    clip.channels = 1;
    const auto frames = static_cast<std::size_t>(std::max(0.0, seconds) * sample_rate);
    clip.samples.resize(frames);
    std::uint64_t state = seed ? seed : 0x9E3779B97F4A7C15ull;
    for (float& s : clip.samples) {
        state ^= state >> 12; state ^= state << 25; state ^= state >> 27; // xorshift64*
        const std::uint64_t r = state * 0x2545F4914F6CDD1Dull;
        s = amplitude * (static_cast<float>(r >> 40) / 8388608.0f - 1.0f); // 24 бита → [−1, 1)
    }
    return clip;
}

} // namespace AudioSystem
