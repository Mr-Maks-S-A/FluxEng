#pragma once
/**
 * @file Texture.hpp
 * @brief RAII-обёртка 2D-текстуры OpenGL (RGBA8).
 */

#include <RendererSystem/Core/Image.hpp>

#include <cstdint>

namespace RendererSystem::GL {

/// @brief Фильтрация текстуры.
enum class TextureFilter : std::uint8_t {
    Nearest, ///< Без сглаживания — чёткие пиксели (пиксель-арт, воксельные атласы).
    Linear,  ///< Билинейное сглаживание.
};

/// @brief Поведение UV за пределами 0..1.
enum class TextureWrap : std::uint8_t {
    ClampToEdge, ///< Край растягивается (нет «швов» у атласов).
    Repeat,      ///< Текстура повторяется (фоны, тайловые поверхности).
};

/**
 * @brief Параметры создания текстуры.
 */
struct TextureDesc {
    TextureFilter filter = TextureFilter::Nearest; ///< Фильтрация (по умолчанию — для пиксель-арта).
    TextureWrap wrap = TextureWrap::ClampToEdge;   ///< Повторение.
    bool mipmaps = false;                          ///< Строить mip-уровни.
};

/**
 * @brief Текстура RGBA8 в видеопамяти.
 *
 * Строки загружаются в порядке Image (строка 0 — верх), поэтому UV (0, 0) —
 * левый верхний пиксель изображения. Это согласовано с осью Y вниз во всём модуле.
 *
 * @pre Все методы требуют текущий OpenGL-контекст.
 */
class Texture {
public:
    /**
     * @brief Загружает изображение в видеопамять.
     * @throws RendererError Если изображение пустое.
     */
    [[nodiscard]] static Texture create(const Image& image, const TextureDesc& desc = {});

    /**
     * @brief Пустая текстура заданного размера (например цель Framebuffer).
     * @throws RendererError Если размер не положительный.
     */
    [[nodiscard]] static Texture create_empty(int width, int height, const TextureDesc& desc = {});

    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    /**
     * @brief Заменяет содержимое (размер должен совпадать) — для динамических атласов.
     * @throws RendererError Если размер отличается.
     */
    void update(const Image& image);

    /// @brief Привязывает текстуру к текстурному блоку `unit`.
    void bind(std::uint32_t unit = 0) const noexcept;

    /// @brief Идентификатор OpenGL.
    [[nodiscard]] std::uint32_t id() const noexcept { return m_id; }
    /// @brief Ширина, пиксели.
    [[nodiscard]] int width() const noexcept { return m_width; }
    /// @brief Высота, пиксели.
    [[nodiscard]] int height() const noexcept { return m_height; }
    /// @brief Параметры создания.
    [[nodiscard]] const TextureDesc& desc() const noexcept { return m_desc; }

private:
    Texture(std::uint32_t id, int width, int height, const TextureDesc& desc) noexcept
        : m_id(id), m_width(width), m_height(height), m_desc(desc) {}

    std::uint32_t m_id = 0;
    int m_width = 0;
    int m_height = 0;
    TextureDesc m_desc{};
};

} // namespace RendererSystem::GL
