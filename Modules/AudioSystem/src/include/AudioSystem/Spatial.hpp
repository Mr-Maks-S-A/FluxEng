#pragma once
/**
 * @file Spatial.hpp
 * @brief 3D-расчёт: расстояние, затухание, конус, пан, Доплер, срез высоких — чистая функция без состояния.
 *
 * Движок вызывает её раз в блок для каждого 3D-голоса. Она же доступна игре (отладочная отрисовка слышимости,
 * «слышит ли охранник игрока») и тестам.
 */

#include <AudioSystem/Types.hpp>

namespace AudioSystem {

struct SpatialResult {
    float gain = 1.0f;         ///< общий множитель громкости (расстояние · конус · перекрытие)
    float pan = 0.0f;          ///< −1 … +1
    float pitch = 1.0f;        ///< множитель из-за эффекта Доплера
    float cutoff_hz = 20000.0f; ///< частота среза фильтра (20000 — фильтр не нужен)
    float distance = 0.0f;
};

/// @brief Затухание по модели; `distance` вне [min, max] зажимается.
[[nodiscard]] float attenuation(AttenuationModel model, float distance, float min_distance, float max_distance, float rolloff) noexcept;

[[nodiscard]] SpatialResult compute_spatial(const Spatial& source, const Listener& listener) noexcept;

} // namespace AudioSystem
