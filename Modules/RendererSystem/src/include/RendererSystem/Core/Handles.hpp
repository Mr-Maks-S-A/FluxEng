#pragma once
/**
 * @file Handles.hpp
 * @brief Строго типизированные дескрипторы ресурсов.
 */

#include <compare>
#include <cstdint>
#include <limits>

namespace RendererSystem {

/**
 * @brief Дескриптор текстуры, выданный Renderer2D.
 *
 * Это индекс, а не указатель: его можно хранить в компонентах ECS,
 * сохранять и сортировать. Индекс 0 всегда занят белой текстурой 1×1,
 * через неё рисуются залитые прямоугольники и линии.
 */
struct TextureHandle {
    std::uint32_t index = 0; ///< Индекс текстуры в Renderer2D.

    /// @brief Встроенная белая текстура 1×1.
    [[nodiscard]] static constexpr TextureHandle white() noexcept { return TextureHandle{0}; }

    friend constexpr bool operator==(TextureHandle, TextureHandle) noexcept = default;
    friend constexpr auto operator<=>(TextureHandle, TextureHandle) noexcept = default;
};

/**
 * @brief Дескриптор шрифта, выданный Renderer2D::add_font().
 */
struct FontHandle {
    /// @brief Значение «шрифт не задан».
    static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index = invalid_index; ///< Индекс шрифта в Renderer2D.

    /// @brief `true`, если дескриптор указывает на шрифт.
    [[nodiscard]] constexpr bool valid() const noexcept { return index != invalid_index; }

    friend constexpr bool operator==(FontHandle, FontHandle) noexcept = default;
};

/**
 * @brief Дескриптор клипа анимации в AnimationLibrary.
 */
struct ClipId {
    /// @brief Значение «клип не задан».
    static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index = invalid_index; ///< Индекс клипа в библиотеке.

    /// @brief `true`, если дескриптор указывает на клип.
    [[nodiscard]] constexpr bool valid() const noexcept { return index != invalid_index; }

    friend constexpr bool operator==(ClipId, ClipId) noexcept = default;
    friend constexpr auto operator<=>(ClipId, ClipId) noexcept = default;
};

} // namespace RendererSystem
