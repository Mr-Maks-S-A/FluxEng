#pragma once
/**
 * @file Clip.hpp
 * @brief Звуковой клип в памяти (float, чередование каналов), декодеры WAV и OGG Vorbis, запись WAV, генераторы для тестов.
 *
 * Клип хранится как `float` в диапазоне −1…+1, 1 или 2 канала (многоканальные файлы сводятся к стерео). Частота дискретизации —
 * родная частота файла: движок сам подстраивает шаг чтения под свою частоту, перекодировать заранее не нужно.
 *
 * Декодер WAV не доверяет данным: размеры берутся из заголовка и проверяются до выделения памяти (`DecodeLimits`),
 * обрезанный или враждебный файл даёт ошибку с причиной, а не падение. OGG (stb_vorbis) — только доверенные данные, см. decode_ogg. Файлов и платформы здесь нет — работа с байтами;
 * чтение с диска/из пака делает вызывающий (например, AssetSystem::Vfs).
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace AudioSystem {

struct AudioClip {
    std::vector<float> samples;       ///< кадры подряд: L R L R … (или M M M …)
    std::uint32_t sample_rate = 0;    ///< Гц
    std::uint16_t channels = 0;       ///< 1 или 2
    std::uint64_t loop_begin = 0;     ///< кадр, с которого повторяется зацикленный клип (интро до него проигрывается один раз)
    std::uint64_t loop_end = 0;       ///< кадр после последнего повторяемого; 0 — до конца клипа

    [[nodiscard]] std::uint64_t frames() const noexcept { return channels == 0 ? 0 : samples.size() / channels; }
    [[nodiscard]] double duration() const noexcept { return sample_rate == 0 ? 0.0 : static_cast<double>(frames()) / sample_rate; }
    [[nodiscard]] bool valid() const noexcept { return channels >= 1 && channels <= 2 && sample_rate > 0 && !samples.empty(); }
    /// @brief Границы повтора с учётом нулей: [begin, end).
    [[nodiscard]] std::uint64_t effective_loop_end() const noexcept {
        const std::uint64_t n = frames();
        return (loop_end == 0 || loop_end > n) ? n : loop_end;
    }
    [[nodiscard]] std::uint64_t effective_loop_begin() const noexcept {
        const std::uint64_t end = effective_loop_end();
        return loop_begin < end ? loop_begin : 0;
    }
};

/// @brief Пределы для недоверенных данных: что больше — отказ до выделения памяти.
struct DecodeLimits {
    std::uint64_t max_frames = 48'000ull * 60 * 20; ///< 20 минут при 48 кГц
    std::uint32_t max_sample_rate = 384'000;
};

using DecodeResult = std::expected<AudioClip, std::string>;

/// @brief WAV: PCM 8/16/24/32 бит, float 32/64, WAVE_FORMAT_EXTENSIBLE; точки повтора из блока `smpl`. Каналов до 8 (свод в стерео).
[[nodiscard]] DecodeResult decode_wav(std::span<const std::byte> bytes, const DecodeLimits& limits = {});
/// @brief OGG Vorbis (stb_vorbis).
/// @warning stb_vorbis не рассчитан на враждебные данные: испорченные служебные заголовки могут повредить память (проверено фаззингом).
///          Длина и параметры проверяются до выделения памяти, но этого мало. Декодируйте OGG только из доверенных источников
///          (свои ассеты, пак с проверенной суммой). Моды, сеть и пользовательские файлы — WAV (`decode_wav` проверяет каждый байт).
[[nodiscard]] DecodeResult decode_ogg(std::span<const std::byte> bytes, const DecodeLimits& limits = {});
/// @brief Определяет формат по сигнатуре («RIFF…WAVE» или «OggS») и вызывает нужный декодер.
[[nodiscard]] DecodeResult decode_audio(std::span<const std::byte> bytes, const DecodeLimits& limits = {});

enum class WavFormat : std::uint8_t { Pcm16, Float32 };

/// @brief Клип → байты WAV (для сохранения результата офлайн-рендера и в тестах). Точки повтора пишутся в `smpl`.
[[nodiscard]] std::vector<std::byte> encode_wav(const AudioClip& clip, WavFormat format = WavFormat::Pcm16);

// ---------------------------------------------------------------------------------------------- генераторы

/// @brief Синус `frequency` Гц.
[[nodiscard]] AudioClip make_sine(float frequency, double seconds, std::uint32_t sample_rate = 48'000, float amplitude = 0.5f);
/// @brief Постоянное значение (удобно в тестах: сумма и громкость считаются в уме).
[[nodiscard]] AudioClip make_constant(float value, std::uint64_t frames, std::uint32_t sample_rate = 48'000, std::uint16_t channels = 1);
/// @brief Белый шум; одинаковый `seed` — одинаковые данные на всех платформах.
[[nodiscard]] AudioClip make_noise(double seconds, std::uint32_t sample_rate = 48'000, std::uint64_t seed = 1, float amplitude = 0.5f);

} // namespace AudioSystem
