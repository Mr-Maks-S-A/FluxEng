#pragma once
/**
 * @file Procedural.hpp
 * @brief Шум Перлина (stb_perlin) и процедурные изображения: фактуры, арты, рельеф.
 *
 * Все функции детерминированы: одинаковые аргументы дают одинаковый результат на любой машине
 * и в любом потоке — их можно вызывать из задач JobSystem.
 */

#include <RendererSystem/Core/Color.hpp>
#include <RendererSystem/Core/Image.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <initializer_list>
#include <span>

namespace RendererSystem::Procedural {

/// @brief Шум Перлина, примерно −1…1. `seed` (0…255) выбирает одну из 256 независимых «вселенных».
[[nodiscard]] float perlin(glm::vec3 point, std::uint8_t seed = 0) noexcept;

/// @brief Фрактальный шум (fBm): сумма `octaves` октав, примерно −1…1.
[[nodiscard]] float fbm(glm::vec3 point, int octaves = 5, float lacunarity = 2.0f, float gain = 0.5f) noexcept;

/// @brief «Гребни» (ridged multifractal), 0…~1: горные хребты, прожилки, молнии.
[[nodiscard]] float ridge(glm::vec3 point, int octaves = 5, float lacunarity = 2.0f, float gain = 0.5f,
                          float offset = 1.0f) noexcept;

/// @brief Турбулентность (сумма |шума|), 0…~1: дым, облака, огонь.
[[nodiscard]] float turbulence(glm::vec3 point, int octaves = 5, float lacunarity = 2.0f, float gain = 0.5f) noexcept;

/// @brief Опорная точка градиента: на позиции `at` (0…1) цвет `color`.
struct GradientStop {
    float at = 0.0f;              ///< Положение 0…1 (по возрастанию).
    Color color = Colors::black;  ///< Цвет.
};

/// @brief Цвет градиента в точке `t` (линейная интерполяция между соседними опорами).
[[nodiscard]] Color sample_gradient(std::span<const GradientStop> stops, float t) noexcept;

/// @brief Вид шума для noise_image().
enum class NoiseKind : std::uint8_t { Fbm, Ridge, Turbulence };

/// @brief Параметры noise_image().
struct NoiseImageDesc {
    int width = 128;                 ///< Ширина, пиксели.
    int height = 128;                ///< Высота, пиксели.
    float scale = 4.0f;              ///< Сколько «ячеек» шума на ширину изображения.
    float z = 0.0f;                  ///< Срез по третьей оси: разные `z` — разные картинки (как seed).
    int octaves = 5;                 ///< Октавы.
    NoiseKind kind = NoiseKind::Fbm; ///< Вид шума.
};

/**
 * @brief Изображение из шума, раскрашенное градиентом.
 * @code
 * const GradientStop fire[] = {{0.0f, Colors::black}, {0.5f, Colors::red}, {1.0f, Colors::yellow}};
 * Image art = Procedural::noise_image({.width = 128, .height = 96, .z = 7.0f}, fire);
 * @endcode
 */
[[nodiscard]] Image noise_image(const NoiseImageDesc& desc, std::span<const GradientStop> gradient);

/// @copydoc noise_image
[[nodiscard]] inline Image noise_image(const NoiseImageDesc& desc, std::initializer_list<GradientStop> gradient) {
    return noise_image(desc, std::span<const GradientStop>(gradient.begin(), gradient.size()));
}

/**
 * @brief Круг (или кольцо) со сглаженным краем — значки, кристаллы маны, частицы.
 * @param size      Сторона изображения.
 * @param fill      Цвет заливки.
 * @param thickness Толщина кольца в пикселях; 0 — сплошной круг.
 */
[[nodiscard]] Image circle_image(int size, Color fill, float thickness = 0.0f);

} // namespace RendererSystem::Procedural
