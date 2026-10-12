#pragma once
/**
 * @file Engine.hpp
 * @brief AudioEngine — микшер: голоса, дерево шин, 3D, реверберация, ducking, лимитер.
 *
 * ```
 *  игра (любой поток)                                   аудиопоток / устройство / тест
 *  ──────────────────                                   ─────────────────────────────
 *  engine.play(clip, params)  ──► очередь команд ──►    engine.render(буфер)
 *  engine.set_bus_volume(...)      (SPSC, без блокировок     ├─ применить команды
 *  engine.set_listener(...)         на стороне рендера)      ├─ смешать голоса → шины → master
 *                                                            └─ реверберация, ducking, лимитер
 * ```
 *
 * Управляющие методы можно звать из любых потоков (внутри один мьютекс, он не пересекается с `render`). `render` зовёт один поток
 * (аудиоколбэк устройства, или тест, или офлайн-рендер) и не блокируется, не выделяет память и не бросает исключений.
 * Результат `render` зависит только от потока команд и числа отрендеренных кадров, поэтому офлайн-рендер воспроизводим.
 *
 * Команды вступают в силу на границе следующего блока (до `block_frames` кадров, ≈5 мс при 48 кГц).
 */

#include <AudioSystem/Clip.hpp>
#include <AudioSystem/Types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AudioSystem {

/// @brief Шина — узел дерева микширования. Родитель должен стоять в списке раньше потомка (0 — master).
struct BusConfig {
    std::string name;
    std::uint16_t parent = 0;
    float volume = 1.0f;
};

/// @brief Правило ducking: пока на шине `source` громко, шина `target` приглушается (музыка под голосом диктора).
struct DuckRule {
    std::uint16_t source = 3;
    std::uint16_t target = 2;
    float reduction_db = 9.0f;  ///< на сколько приглушить
    float threshold = 0.02f;    ///< пиковый уровень источника, с которого срабатывает
    float attack_seconds = 0.05f;
    float release_seconds = 0.6f;
};

/// @brief Стандартная раскладка: master, sfx, music, voice, ambient, ui — как `Buses::*`.
[[nodiscard]] std::vector<BusConfig> default_buses();

struct EngineConfig {
    std::uint32_t sample_rate = 48'000;
    std::uint32_t max_voices = 64;
    std::uint32_t command_capacity = 2048; ///< очередь команд; переполнение — команды теряются и считаются в `Stats::dropped_commands`
    std::uint32_t block_frames = 256;      ///< внутренний шаг обработки: параметры плавно меняются на этом интервале
    float limiter_ceiling = 0.98f;
    std::vector<BusConfig> buses = default_buses();
    std::vector<DuckRule> ducking = {DuckRule{}}; ///< по умолчанию: голос приглушает музыку
};

struct BusMeter {
    float peak = 0.0f; ///< максимум |выхода шины| в последнем блоке
    float rms = 0.0f;
};

struct Stats {
    std::uint64_t frames_rendered = 0;
    std::uint64_t dropped_commands = 0; ///< очередь переполнена
    std::uint64_t dropped_voices = 0;   ///< не хватило голосов и приоритет новой не выше
    std::uint64_t stolen_voices = 0;    ///< вытеснено более важным звуком
    std::uint64_t limiter_blocks = 0;   ///< сколько блоков сработал лимитер
};

class AudioEngine {
public:
    explicit AudioEngine(EngineConfig config = {});
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return m_config.sample_rate; }
    [[nodiscard]] const EngineConfig& config() const noexcept { return m_config; }

    // ------------------------------------------------------------------ клипы

    /// @brief Зарегистрировать клип под именем. Повторное имя заменяет клип (играющие голоса доиграют старый). Невалидный клип → ClipId{}.
    ClipId add_clip(std::string name, AudioClip clip);
    [[nodiscard]] ClipId find_clip(std::string_view name) const;
    /// @brief Забыть клип: играющие голоса доигрывают (держат данные сами), новые запустить нельзя.
    void remove_clip(ClipId id);
    [[nodiscard]] std::shared_ptr<const AudioClip> clip(ClipId id) const;

    // ------------------------------------------------------------------ голоса

    /// @brief Запустить звук. Нулевой VoiceId — отказ (нет клипа, очередь полна).
    VoiceId play(ClipId clip, const PlayParams& params = {});
    /// @brief Остановить; `fade_seconds` > 0 — плавно.
    void stop(VoiceId voice, float fade_seconds = 0.0f);
    /// @brief Остановить всё на шине и её потомках.
    void stop_bus(BusId bus, float fade_seconds = 0.0f);
    void stop_all(float fade_seconds = 0.0f);
    void pause(VoiceId voice, bool paused);
    void set_voice_volume(VoiceId voice, float volume, float ramp_seconds = 0.0f);
    void set_voice_pitch(VoiceId voice, float pitch);
    void set_voice_pan(VoiceId voice, float pan);
    /// @brief Обновить все 3D-параметры (обычно каждый кадр вместе с позицией объекта).
    void set_spatial(VoiceId voice, const Spatial& spatial);
    void set_position(VoiceId voice, Vec3 position, Vec3 velocity = {});
    /// @brief Играет ли голос (или ещё стоит в очереди на запуск).
    [[nodiscard]] bool is_playing(VoiceId voice) const noexcept;
    [[nodiscard]] std::uint32_t active_voices() const noexcept;

    // ------------------------------------------------------------------ шины

    [[nodiscard]] BusId find_bus(std::string_view name) const;
    [[nodiscard]] std::size_t bus_count() const noexcept { return m_config.buses.size(); }
    void set_bus_volume(BusId bus, float volume, float fade_seconds = 0.0f);
    void set_bus_mute(BusId bus, bool muted);
    /// @brief Срез высоких на шине (Гц); ≥ 20000 — выключен. Подводный уровень, оглушение.
    void set_bus_lowpass(BusId bus, float cutoff_hz);
    /// @brief Пауза: голоса шины и потомков замирают (позиция не двигается), хвост реверберации доигрывает.
    void pause_bus(BusId bus, bool paused);
    [[nodiscard]] BusMeter bus_meter(BusId bus) const noexcept;
    void set_master_volume(float volume, float fade_seconds = 0.0f) { set_bus_volume(Buses::master, volume, fade_seconds); }

    // ------------------------------------------------------------------ слушатель, реверберация

    void set_listener(const Listener& listener);
    void set_reverb(const ReverbParams& params);

    // ------------------------------------------------------------------ аудиопоток

    /// @brief Смешать `buffer.size()/2` кадров стерео (L R L R …) в `buffer` (перезаписывает). Размер должен быть чётным.
    void render(std::span<float> buffer) noexcept;

    [[nodiscard]] Stats stats() const noexcept;
    /// @brief Время в секундах по числу отрендеренных кадров (часы звука: не зависят от стены, воспроизводимы офлайн).
    [[nodiscard]] double time_seconds() const noexcept;

private:
    struct Impl;
    EngineConfig m_config;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AudioSystem
