#include <AudioSystem/Clip.hpp>

#include <algorithm>
#include <climits>
#include <memory>
#include <string>

#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wconversion"
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#    pragma GCC diagnostic ignored "-Wshadow"
#    pragma GCC diagnostic ignored "-Wdouble-promotion"
#    pragma GCC diagnostic ignored "-Wunused-function"
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#    pragma GCC diagnostic ignored "-Wold-style-cast"
#    pragma GCC diagnostic ignored "-Wcast-qual"
#    pragma GCC diagnostic ignored "-Wpedantic"
#    pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#    pragma GCC diagnostic ignored "-Wcomment"
#    pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
// Только память (без FILE*), только pull-API; функции видны этому файлу, чтобы не конфликтовать с другой копией stb_vorbis в программе.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include <stb_vorbis.c>
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

namespace AudioSystem {

namespace {
struct VorbisCloser {
    stb_vorbis* handle;
    ~VorbisCloser() { if (handle) stb_vorbis_close(handle); }
};
} // namespace

DecodeResult decode_ogg(std::span<const std::byte> bytes, const DecodeLimits& limits) {
    if (bytes.empty()) return std::unexpected("ogg: empty data");
    if (bytes.size() > static_cast<std::size_t>(INT_MAX)) return std::unexpected("ogg: data is larger than 2 GiB");

    // Всю память разбора stb_vorbis берёт из нашего буфера, а не из malloc: потолок задан, утечек при ошибке нет (буфер освободим сами),
    // а испорченный заголовок, требующий гигабайты, получает «нет памяти» вместо попытки выделения.
    constexpr int setup_memory_cap = 16 * 1024 * 1024;
    const std::unique_ptr<char[]> setup_memory(new char[setup_memory_cap]);
    stb_vorbis_alloc allocator{};
    allocator.alloc_buffer = setup_memory.get();
    allocator.alloc_buffer_length_in_bytes = setup_memory_cap;

    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), &error, &allocator);
    if (!vorbis) return std::unexpected("ogg: cannot open stream (stb_vorbis error " + std::to_string(error) + ")");
    VorbisCloser closer{vorbis};

    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    if (info.channels < 1 || info.channels > 8) return std::unexpected("ogg: unsupported channel count");
    if (info.sample_rate < 1000 || info.sample_rate > limits.max_sample_rate) return std::unexpected("ogg: sample rate is out of range");

    // Длину узнаём до выделения памяти: враждебный файл не должен дойти до гигабайтного resize.
    const unsigned total = stb_vorbis_stream_length_in_samples(vorbis);
    if (total == 0) return std::unexpected("ogg: no audio frames");
    if (total > limits.max_frames) return std::unexpected("ogg: clip is longer than the allowed limit");

    const auto channels = static_cast<std::size_t>(info.channels);
    std::vector<float> interleaved(static_cast<std::size_t>(total) * channels);
    std::size_t frames_done = 0;
    while (frames_done < total) {
        const std::size_t want = std::min<std::size_t>(total - frames_done, 4096);
        const int got = stb_vorbis_get_samples_float_interleaved(vorbis, info.channels, interleaved.data() + frames_done * channels,
                                                                 static_cast<int>(want * channels));
        if (got <= 0) break;
        frames_done += static_cast<std::size_t>(got);
    }
    if (frames_done == 0) return std::unexpected("ogg: stream decodes to nothing");
    interleaved.resize(frames_done * channels);

    AudioClip clip;
    clip.sample_rate = info.sample_rate;
    clip.channels = static_cast<std::uint16_t>(channels);
    clip.samples = std::move(interleaved);
    if (clip.channels > 2) {
        // Тот же свод, что и у WAV: через повторный декод WAV-кодека не пройти, поэтому усредняем здесь.
        const std::size_t frames = clip.samples.size() / clip.channels;
        std::vector<float> stereo(frames * 2);
        const float left_count = static_cast<float>((clip.channels + 1u) / 2u);
        const float right_count = static_cast<float>(clip.channels / 2u);
        for (std::size_t f = 0; f < frames; ++f) {
            float l = 0.0f, r = 0.0f;
            for (std::size_t c = 0; c < clip.channels; ++c) (c % 2 == 0 ? l : r) += clip.samples[f * clip.channels + c];
            stereo[f * 2] = l / left_count;
            stereo[f * 2 + 1] = r / right_count;
        }
        clip.samples = std::move(stereo);
        clip.channels = 2;
    }
    return clip;
}

} // namespace AudioSystem
