#pragma once
/**
 * @file Backend.hpp
 * @brief Куда уходит звук: интерфейс бэкенда, «пустой» бэкенд без устройства и офлайн-рендер.
 *
 * AudioEngine сам ничего не играет — он умеет `render(буфер)`. Бэкенд отдаёт этот буфер устройству вывода
 * (WASAPI, CoreAudio, ALSA/PulseAudio, miniaudio, SDL) из его аудиоколбэка. Платформенный бэкенд — отдельная, тонкая реализация
 * `AudioBackend`; сам движок, тесты и сервер без звуковой карты от неё не зависят.
 */

#include <AudioSystem/Engine.hpp>

#include <expected>
#include <string>
#include <vector>

namespace AudioSystem {

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    /// @brief Открыть устройство и начать звать `engine.render` из аудиопотока. Движок должен пережить бэкенд.
    virtual std::expected<void, std::string> start(AudioEngine& engine) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

/// @brief Бэкенд без устройства: кадры рендерятся вручную (`pump`). Для сервера, тестов и игр без звука.
class NullBackend final : public AudioBackend {
public:
    std::expected<void, std::string> start(AudioEngine& engine) override { m_engine = &engine; return {}; }
    void stop() noexcept override { m_engine = nullptr; }
    [[nodiscard]] const char* name() const noexcept override { return "null"; }

    /// @brief Отрендерить `frames` кадров и вернуть стерео-буфер (живёт до следующего `pump`). Без `start` — тишина.
    const std::vector<float>& pump(std::size_t frames);

private:
    AudioEngine* m_engine = nullptr;
    std::vector<float> m_buffer;
};

/// @brief Отрендерить `seconds` секунд подряд (интерливленное стерео). Воспроизводимо: те же команды — те же числа.
[[nodiscard]] std::vector<float> render_offline(AudioEngine& engine, double seconds);

/// @brief Отрендерить в клип (стерео, частота движка) — например, чтобы сохранить `encode_wav`'ом и послушать.
[[nodiscard]] AudioClip render_to_clip(AudioEngine& engine, double seconds);

} // namespace AudioSystem
