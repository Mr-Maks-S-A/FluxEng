#pragma once
/**
 * @file Device.hpp
 * @brief Устройство вывода звука (SDL3): поток вывода берёт кадры у Mixer. Публичный заголовок без SDL.
 *
 * Микшер вызывается из потока звука SDL, игра — из главного: доступ к нему идёт под коротким замком. Через `Device`
 * удобно запускать звуки; `with_mixer` даёт сделать несколько вызовов подряд атомарно.
 *
 * Драйвер выбирает SDL (PipeWire, PulseAudio, ALSA, WASAPI, CoreAudio …); `SDL_AUDIODRIVER=dummy` — устройство без
 * звука (CI): время идёт, микшер работает, слышно ничего. Если устройства нет совсем, `create` возвращает ошибку,
 * а игра обычно продолжает без звука.
 */

#include <AudioSystem/Mixer.hpp>

#include <expected>
#include <memory>
#include <string>

namespace AudioSystem {

struct DeviceConfig {
    int sample_rate = 48000;
    int buffer_frames = 512; ///< Подсказка SDL о размере буфера: меньше — меньше задержка и выше риск щелчков.
    std::size_t max_voices = 32;
};

class Device {
public:
    [[nodiscard]] static std::expected<Device, std::string> create(const DeviceConfig& config = {});

    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    ~Device();

    [[nodiscard]] int sample_rate() const noexcept;
    /// @brief Имя драйвера SDL («pipewire», «dummy» …).
    [[nodiscard]] std::string driver_name() const;

    void pause() noexcept;
    void resume() noexcept;

    VoiceId play(std::shared_ptr<const Wave> wave, PlayParams params = {});
    VoiceId note_on(const ToneParams& params);
    VoiceId play_note(const ToneParams& params, float hold_seconds);
    void release(VoiceId id) noexcept;
    void stop_all() noexcept;
    void set_master_volume(float volume) noexcept;
    [[nodiscard]] std::size_t active_voices() const;
    /// @brief Кадров, уже отданных устройству (по ним видно, что звук «идёт»).
    [[nodiscard]] std::uint64_t frames_rendered() const;

    /// @brief Вызвать `fn(Mixer&)` под замком звукового потока.
    template<typename Fn>
    decltype(auto) with_mixer(Fn&& fn) {
        Lock lock = acquire();
        return fn(mixer());
    }

private:
    struct Impl;
    explicit Device(std::unique_ptr<Impl> impl) noexcept;

    /// Замок на время вызова with_mixer (скрывает std::mutex и SDL из заголовка).
    struct Lock {
        Impl* impl;
        explicit Lock(Impl* i);
        ~Lock();
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    };
    Lock acquire() { return Lock(m_impl.get()); }
    Mixer& mixer() noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace AudioSystem
