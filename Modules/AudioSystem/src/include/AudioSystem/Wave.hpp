#pragma once
/**
 * @file Wave.hpp
 * @brief Звук в памяти: PCM-буфер, простые синтезированные тона, чтение и запись WAV. Платформы (SDL) здесь нет.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace AudioSystem {

/// @brief Форма волны генератора.
enum class Shape : std::uint8_t { Sine, Square, Saw, Triangle, Noise };

/// @brief Звук: чередующиеся (interleaved) отсчёты float −1…+1. Каналов 1 (моно) или 2 (стерео).
struct Wave {
    int channels = 1;
    int sample_rate = 48000;
    std::vector<float> samples;

    [[nodiscard]] std::size_t frames() const noexcept { return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0; }
    [[nodiscard]] double seconds() const noexcept { return sample_rate > 0 ? static_cast<double>(frames()) / sample_rate : 0.0; }
};

/// @brief Тон заданной формы и длины, с коротким затуханием в конце (без щелчка). Моно.
[[nodiscard]] Wave make_tone(Shape shape, float frequency_hz, float seconds, int sample_rate = 48000, float amplitude = 0.5f);

/// @brief Всплеск шума с экспоненциальным затуханием: барабан, щелчок, шаг. Детерминированный (seed). Моно.
[[nodiscard]] Wave make_noise_burst(float seconds, float decay_per_second = 18.0f, int sample_rate = 48000, float amplitude = 0.6f, std::uint32_t seed = 1);

/// @brief Разобрать WAV из памяти: PCM 8/16/24/32 бит и float 32, 1–2 канала. Остальное — сообщение об ошибке, не исключение.
[[nodiscard]] std::expected<Wave, std::string> decode_wav(std::span<const std::uint8_t> bytes);
/// @brief WAV PCM 16 бит (с ограничением амплитуды −1…+1).
[[nodiscard]] std::vector<std::uint8_t> encode_wav(const Wave& wave);

[[nodiscard]] std::expected<Wave, std::string> load_wav(const std::filesystem::path& path);
[[nodiscard]] std::expected<void, std::string> save_wav(const std::filesystem::path& path, const Wave& wave);

} // namespace AudioSystem
