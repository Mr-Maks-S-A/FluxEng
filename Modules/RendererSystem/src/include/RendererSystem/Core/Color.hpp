#pragma once
/**
 * @file Color.hpp
 * @brief Цвет RGBA8 — формат, в котором цвет хранится в вершинах и текстурах.
 */

#include <glm/vec4.hpp>

#include <algorithm>
#include <cstdint>

namespace RendererSystem {

/**
 * @brief Цвет из четырёх байт в порядке R, G, B, A.
 *
 * Именно в таком виде цвет лежит в вершине (4 байта вместо 16 у `vec4`)
 * и в пикселях Image/Texture. В шейдер попадает нормализованным `vec4`.
 */
struct Color {
    std::uint8_t r = 255; ///< Красный.
    std::uint8_t g = 255; ///< Зелёный.
    std::uint8_t b = 255; ///< Синий.
    std::uint8_t a = 255; ///< Альфа (255 — непрозрачный).

    /**
     * @brief Цвет из шестнадцатеричной записи `0xRRGGBBAA`.
     * @code Color::from_rgba(0xFF8000FF) // оранжевый @endcode
     */
    [[nodiscard]] static constexpr Color from_rgba(std::uint32_t rgba) noexcept {
        return Color{static_cast<std::uint8_t>(rgba >> 24), static_cast<std::uint8_t>(rgba >> 16),
                     static_cast<std::uint8_t>(rgba >> 8), static_cast<std::uint8_t>(rgba)};
    }

    /// @brief Цвет из компонент 0..1 (значения за пределами обрезаются).
    [[nodiscard]] static constexpr Color from_floats(float red, float green, float blue, float alpha = 1.0f) noexcept {
        return Color{to_byte(red), to_byte(green), to_byte(blue), to_byte(alpha)};
    }

    /// @brief Запись `0xRRGGBBAA`.
    [[nodiscard]] constexpr std::uint32_t to_rgba() const noexcept {
        return (std::uint32_t{r} << 24) | (std::uint32_t{g} << 16) | (std::uint32_t{b} << 8) | std::uint32_t{a};
    }

    /// @brief Компоненты 0..1.
    [[nodiscard]] constexpr glm::vec4 to_vec4() const noexcept {
        return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    /// @brief Тот же цвет с другой альфой.
    [[nodiscard]] constexpr Color with_alpha(std::uint8_t alpha) const noexcept { return Color{r, g, b, alpha}; }

    /// @brief Покомпонентное умножение (тонирование), как в шейдере.
    [[nodiscard]] constexpr Color modulate(Color other) const noexcept {
        return Color{mul(r, other.r), mul(g, other.g), mul(b, other.b), mul(a, other.a)};
    }

    constexpr bool operator==(const Color&) const noexcept = default;

private:
    static constexpr std::uint8_t to_byte(float value) noexcept {
        return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    static constexpr std::uint8_t mul(std::uint8_t x, std::uint8_t y) noexcept {
        return static_cast<std::uint8_t>((unsigned{x} * unsigned{y} + 127u) / 255u);
    }
};

static_assert(sizeof(Color) == 4, "Color must stay 4 bytes: it is a vertex attribute");

/// @brief Именованные цвета.
namespace Colors {
inline constexpr Color white{255, 255, 255, 255};       ///< Белый.
inline constexpr Color black{0, 0, 0, 255};             ///< Чёрный.
inline constexpr Color transparent{0, 0, 0, 0};         ///< Полностью прозрачный.
inline constexpr Color red{255, 0, 0, 255};             ///< Красный.
inline constexpr Color green{0, 255, 0, 255};           ///< Зелёный.
inline constexpr Color blue{0, 0, 255, 255};            ///< Синий.
inline constexpr Color yellow{255, 255, 0, 255};        ///< Жёлтый.
inline constexpr Color magenta{255, 0, 255, 255};       ///< Пурпурный (цвет «нет текстуры»).
} // namespace Colors

} // namespace RendererSystem
