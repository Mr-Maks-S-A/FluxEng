#pragma once
/**
 * @file SoundEvents.hpp
 * @brief Звуковые события: игра говорит «шаг по траве», а какой клип, с какой громкостью и тональностью — решают данные.
 *
 * Событие — именованное описание: набор клипов на выбор (без подряд повторяющегося), разброс громкости и высоты, шина,
 * 3D-параметры, приоритет, лимит одновременных экземпляров и «остывание». Описания живут в тексте (`load_text`/`to_text`),
 * поэтому звукорежиссёр правит их без перекомпиляции, а программист пишет `events.post("footstep", position)`.
 *
 * Случайность — собственный генератор с зерном: одинаковые вызовы дают одинаковый результат (воспроизводимые тесты и офлайн-рендер).
 * Время «остывания» — часы звука (`AudioEngine::time_seconds`), а не стена.
 *
 * Формат текста (`#` — комментарий; ключи не обязательны):
 * ```
 * [footstep]
 * clips = step_1 step_2 step_3
 * bus = sfx
 * volume = 0.8
 * volume_var = 0.1        # ± случайный разброс
 * pitch = 1.0
 * pitch_var = 1.5         # ± полутонов
 * spatial = true
 * min_distance = 2
 * max_distance = 30
 * model = inverse         # none | inverse | linear | exponential
 * max_instances = 4       # 0 — без предела
 * cooldown = 0.08         # секунд между запусками
 * ```
 */

#include <AudioSystem/Engine.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AudioSystem {

struct SoundEventDef {
    std::string name;
    std::vector<std::string> clips;  ///< имена клипов, зарегистрированных в движке; ищутся при запуске
    std::string bus = "sfx";
    float volume = 1.0f;
    float volume_var = 0.0f;         ///< ±, линейно
    float pitch = 1.0f;
    float pitch_var = 0.0f;          ///< ± полутонов
    bool loop = false;
    bool spatial = false;
    float min_distance = 1.0f;
    float max_distance = 50.0f;
    float rolloff = 1.0f;
    AttenuationModel model = AttenuationModel::Inverse;
    float doppler = 1.0f;
    float air_absorption = 0.0f;
    std::uint8_t priority = 128;
    std::uint32_t max_instances = 0; ///< 0 — без предела; при пределе новый экземпляр не запускается
    float cooldown = 0.0f;           ///< секунд между запусками этого события
    float reverb_send = 0.0f;
    float fade_in = 0.0f;
    bool avoid_repeat = true;        ///< не выбирать подряд один и тот же клип
};

class SoundEvents {
public:
    explicit SoundEvents(AudioEngine& engine, std::uint64_t seed = 1) noexcept : m_engine(&engine), m_rng(seed ? seed : 1) {}

    /// @brief Добавить или заменить событие. Ложь — пустое имя или нет клипов.
    bool add(SoundEventDef def);
    [[nodiscard]] const SoundEventDef* find(std::string_view name) const;
    [[nodiscard]] std::size_t size() const noexcept { return m_defs.size(); }

    /// @brief Разобрать текст описаний; возвращает число добавленных событий. Ошибка называет строку: «line 7: unknown key 'volum'».
    /// События до ошибки остаются добавленными только при успехе: разбор атомарен.
    std::expected<std::size_t, std::string> load_text(std::string_view text);
    /// @brief Все события в текст (порядок по имени): `load_text(to_text())` даёт то же самое.
    [[nodiscard]] std::string to_text() const;

    /// @brief Запустить событие без 3D. Нулевой VoiceId — не запущено (нет события/клипа, «остывание», предел экземпляров).
    VoiceId post(std::string_view name);
    /// @brief Запустить 3D-событие в точке (если описание не 3D — позиция игнорируется).
    VoiceId post(std::string_view name, Vec3 position, Vec3 velocity = {});
    /// @brief Остановить все экземпляры события.
    void stop(std::string_view name, float fade_seconds = 0.0f);

private:
    struct State {
        SoundEventDef def;
        double last_post = -1e30;
        std::size_t last_clip = static_cast<std::size_t>(-1);
        std::vector<VoiceId> instances;
    };

    VoiceId start(State& state, const Vec3* position, Vec3 velocity);
    float random01() noexcept;

    AudioEngine* m_engine;
    std::uint64_t m_rng;
    std::unordered_map<std::string, State> m_defs;
};

} // namespace AudioSystem
