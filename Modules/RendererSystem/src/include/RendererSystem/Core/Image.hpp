#pragma once
/**
 * @file Image.hpp
 * @brief Изображение RGBA8 в оперативной памяти (без OpenGL).
 */

#include <RendererSystem/Core/Color.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace RendererSystem {

/**
 * @brief Пиксели RGBA8, строки сверху вниз.
 *
 * Строка 0 — верх изображения, как в файле и в редакторе. Texture загружает
 * строки в том же порядке, поэтому UV (0, 0) соответствует левому верхнему пикселю.
 *
 * Изображение не зависит от OpenGL: его можно загружать в фоновом потоке,
 * генерировать процедурно (атласы вокселей, шумы) и проверять в тестах.
 */
class Image {
public:
    /// @brief Пустое изображение 0×0.
    Image() = default;

    /**
     * @brief Изображение заданного размера, залитое цветом.
     * @throws RendererError Если размер отрицательный.
     */
    Image(int width, int height, Color fill = Colors::transparent);

    /**
     * @brief Загружает PNG/JPG/BMP/TGA/… из файла; результат всегда RGBA8.
     * @return Изображение или текст ошибки.
     */
    [[nodiscard]] static std::expected<Image, std::string> load(const std::filesystem::path& path);

    /**
     * @brief Декодирует изображение из памяти (например из архива ресурсов).
     * @return Изображение или текст ошибки.
     */
    [[nodiscard]] static std::expected<Image, std::string> decode(std::span<const std::byte> encoded);

    /**
     * @brief Шахматная доска — удобная отладочная текстура.
     * @param cell Размер клетки в пикселях.
     */
    [[nodiscard]] static Image checkerboard(int width, int height, int cell, Color first, Color second);

    /// @brief Ширина, пиксели.
    [[nodiscard]] int width() const noexcept { return m_width; }
    /// @brief Высота, пиксели.
    [[nodiscard]] int height() const noexcept { return m_height; }
    /// @brief `true`, если пикселей нет.
    [[nodiscard]] bool empty() const noexcept { return m_pixels.empty(); }

    /// @brief Пиксель (без проверки границ в release).
    [[nodiscard]] Color pixel(int x, int y) const noexcept;
    /// @brief Меняет пиксель (без проверки границ в release).
    void set_pixel(int x, int y, Color color) noexcept;
    /// @brief Заливает прямоугольную область (обрезается по границам изображения).
    void fill_rect(int x, int y, int width, int height, Color color) noexcept;

    /// @brief Сырые пиксели: `width * height` цветов подряд, строки сверху вниз.
    [[nodiscard]] std::span<const Color> pixels() const noexcept { return m_pixels; }
    /// @copydoc pixels
    [[nodiscard]] std::span<Color> pixels() noexcept { return m_pixels; }

    /// @brief Переворачивает строки (нужно при чтении из OpenGL, где строка 0 — низ).
    void flip_vertically() noexcept;

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<Color> m_pixels;
};

} // namespace RendererSystem
