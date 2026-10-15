#pragma once
/**
 * @file Mixer.hpp
 * @brief Микшер: голоса-сэмплы и голоса-синтезаторы, огибающая ADSR, панорама, питч, зацикливание.
 *
 * Микшер — чистая функция времени: `render()` дописывает ровно столько кадров, сколько просили, и больше ни от чего
 * не зависит (ни от SDL, ни от часов). Поэтому звук детерминирован, проверяется тестами и пишется в WAV без устройства.
 * `render()` не выделяет память. Потоки — забота Device: сам Mixer не потокобезопасен.
 *
 * Выход — стерео, чередующиеся float; перед записью в буфер он ограничивается значениями −1…+1.
 */

#include <AudioSystem/Wave.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace AudioSystem {

/// @brief ADSR: время нарастания, спада, уровень удержания (0…1) и время затухания, секунды.
struct Envelope {
    float attack = 0.005f;
    float decay = 0.06f;
    float sustain = 0.7f;
    float release = 0.2f;
};

/// @brief Номер голоса. 0 — «нет голоса» (микшер заполнен). Номера не повторяются, поэтому устаревший не попадёт в чужой голос.
struct VoiceId {
    std::uint32_t value = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(VoiceId, VoiceId) noexcept = default;
};

/// @brief Параметры воспроизведения сэмпла.
struct PlayParams {
    float volume = 1.0f;
    float pan = 0.0f;   ///< −1 (левый) … +1 (правый), мощность сохраняется
    float pitch = 1.0f; ///< 2.0 — на октаву выше и вдвое быстрее
    bool loop = false;
};

/// @brief Параметры ноты-синтезатора.
struct ToneParams {
    Shape shape = Shape::Sine;
    float frequency = 440.0f;
    float volume = 0.3f;
    float pan = 0.0f;
    Envelope envelope{};
    float vibrato_hz = 0.0f;    ///< частота вибрато; 0 — нет
    float vibrato_depth = 0.0f; ///< глубина вибрато, полутоны
};

/// @brief Частота ноты MIDI (69 = ля первой октавы = 440 Гц).
[[nodiscard]] float midi_to_hz(float midi_note) noexcept;

class Mixer {
public:
    explicit Mixer(int sample_rate = 48000, std::size_t max_voices = 32);
    Mixer(Mixer&&) noexcept;
    Mixer& operator=(Mixer&&) noexcept;
    ~Mixer();

    [[nodiscard]] int sample_rate() const noexcept { return m_rate; }

    /// @brief Запустить сэмпл. При полном микшере вытесняется самый старый голос.
    VoiceId play(std::shared_ptr<const Wave> wave, PlayParams params = {});
    /// @brief Нота, пока её не отпустят (`release`).
    VoiceId note_on(const ToneParams& params);
    /// @brief Нота фиксированной длины: через `hold_seconds` сама переходит в затухание.
    VoiceId play_note(const ToneParams& params, float hold_seconds);

    /// @brief Начать затухание голоса (нота) или быстро затихнуть (сэмпл). Неизвестный номер — ничего.
    void release(VoiceId id) noexcept;
    /// @brief Оборвать голос сразу.
    void stop(VoiceId id) noexcept;
    void stop_all() noexcept;

    void set_master_volume(float volume) noexcept { m_master = volume < 0.0f ? 0.0f : volume; }
    [[nodiscard]] float master_volume() const noexcept { return m_master; }

    [[nodiscard]] std::size_t active_voices() const noexcept;
    [[nodiscard]] bool is_active(VoiceId id) const noexcept;
    [[nodiscard]] std::uint64_t frames_rendered() const noexcept { return m_frames; }

    /// @brief Заполнить буфер стерео (L, R, L, R …). Размер — чётное число отсчётов.
    void render(std::span<float> stereo) noexcept;

private:
    struct Voice;
    VoiceId start(Voice&& voice);
    Voice* find(VoiceId id) noexcept;
    [[nodiscard]] const Voice* find(VoiceId id) const noexcept;

    int m_rate;
    float m_master = 1.0f;
    std::uint32_t m_next_id = 1;
    std::uint64_t m_frames = 0;
    std::vector<Voice> m_voices;
};

} // namespace AudioSystem
