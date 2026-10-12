#pragma once
/**
 * @file Types.hpp
 * @brief Базовые типы AudioSystem: векторы, идентификаторы, параметры воспроизведения, слушатель, 3D-параметры.
 *
 * Система координат правая: +X вправо, +Y вверх, «вперёд» по умолчанию −Z (как в OpenGL/OpenAL). Единицы — метры, секунды.
 * Идентификаторы (ClipId, VoiceId) — непрозрачные числа; нулевой всегда «нет». Устаревший VoiceId безопасен:
 * он просто не найдётся (счётчик растёт монотонно и не переиспользуется).
 */

#include <cmath>
#include <cstdint>

namespace AudioSystem {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(Vec3 a) noexcept { return std::sqrt(dot(a, a)); }
/// @brief Единичный вектор; для нулевого — `fallback` (направление не определено, но и не NaN).
[[nodiscard]] inline Vec3 normalized(Vec3 a, Vec3 fallback = {0.0f, 0.0f, -1.0f}) noexcept {
    const float len = length(a);
    return len > 1e-6f ? a * (1.0f / len) : fallback;
}

inline constexpr float speed_of_sound = 343.0f; ///< м/с, для эффекта Доплера.

struct ClipId {
    std::uint32_t value = 0;
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return value != 0; }
    friend constexpr bool operator==(ClipId, ClipId) = default;
};

struct VoiceId {
    std::uint32_t value = 0;
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return value != 0; }
    friend constexpr bool operator==(VoiceId, VoiceId) = default;
};

/// @brief Шина — узел дерева микширования. Индекс 0 всегда «master».
struct BusId {
    std::uint16_t index = 1; ///< По умолчанию — «sfx» стандартной раскладки (см. default_buses()).
    friend constexpr bool operator==(BusId, BusId) = default;
};

/// @brief Шины стандартной раскладки `default_buses()`.
namespace Buses {
inline constexpr BusId master{0};
inline constexpr BusId sfx{1};
inline constexpr BusId music{2};
inline constexpr BusId voice{3};
inline constexpr BusId ambient{4};
inline constexpr BusId ui{5};
} // namespace Buses

/// @brief Как громкость падает с расстоянием (параметры — OpenAL-совместимые).
enum class AttenuationModel : std::uint8_t {
    None,        ///< без затухания
    Inverse,     ///< min / (min + rolloff·(d − min)) — «как в жизни»
    Linear,      ///< 1 − rolloff·(d − min)/(max − min): ровно ноль на max_distance
    Exponential, ///< (d / min)^(−rolloff)
};

/// @brief 3D-параметры источника. Игра обновляет position/velocity каждый кадр через `set_spatial`/`set_position`.
struct Spatial {
    bool enabled = false;
    Vec3 position{};
    Vec3 velocity{};
    Vec3 direction{0.0f, 0.0f, -1.0f}; ///< куда «смотрит» источник (для конуса)
    float min_distance = 1.0f;         ///< ближе — полная громкость
    float max_distance = 50.0f;        ///< дальше затухание не растёт (для Linear — тишина)
    float rolloff = 1.0f;
    AttenuationModel model = AttenuationModel::Inverse;
    float cone_inner_degrees = 360.0f; ///< полный угол конуса без ослабления
    float cone_outer_degrees = 360.0f; ///< за пределами — `cone_outer_gain`
    float cone_outer_gain = 0.0f;
    float doppler = 1.0f;              ///< 0 — выключен
    float air_absorption = 0.0f;       ///< 0 — выключено; больше — дальние звуки глуше (срез высоких)
    float occlusion = 0.0f;            ///< 0…1: игра считает сама (луч до слушателя) — глушит и приглушает
};

struct Listener {
    Vec3 position{};
    Vec3 velocity{};
    Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
};

/// @brief Параметры запуска звука.
struct PlayParams {
    BusId bus = Buses::sfx;
    float volume = 1.0f;        ///< линейный множитель, ≥ 0
    float pitch = 1.0f;         ///< 1 — как в файле, 2 — октавой выше (и вдвое быстрее)
    float pan = 0.0f;           ///< −1 (лево) … +1 (право); для 3D-звука считается из позиции
    bool loop = false;
    std::uint8_t priority = 128; ///< при нехватке голосов вытесняется самый низкий приоритет
    float fade_in = 0.0f;        ///< секунд
    float start_delay = 0.0f;    ///< секунд до старта
    float start_offset = 0.0f;   ///< секунд от начала клипа
    float reverb_send = 0.0f;    ///< 0…1: сколько сигнала идёт в реверберацию
    Spatial spatial{};
};

/// @brief Реверберация (Freeverb: 8 гребёнок + 4 all-pass на канал). Одна на движок; голоса отправляют в неё через reverb_send.
struct ReverbParams {
    float room_size = 0.5f; ///< 0…1: длина хвоста
    float damping = 0.5f;   ///< 0…1: затухание высоких
    float wet = 0.0f;       ///< 0…1: уровень возврата; 0 — реверберация выключена (и не считается)
    float width = 1.0f;     ///< 0…1: ширина стерео
};

[[nodiscard]] inline float db_to_gain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
[[nodiscard]] inline float gain_to_db(float gain) noexcept { return 20.0f * std::log10(gain > 1e-9f ? gain : 1e-9f); }

} // namespace AudioSystem
