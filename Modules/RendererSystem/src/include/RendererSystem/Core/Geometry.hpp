#pragma once
/**
 * @file Geometry.hpp
 * @brief Прямоугольники в мире и в текстурных координатах.
 *
 * Соглашение о координатах всего модуля: **ось Y направлена вниз**,
 * (0, 0) — левый верхний угол. Так же устроены пиксели изображений,
 * поэтому координаты спрайт-листов совпадают с координатами в редакторе.
 */

#include <glm/vec2.hpp>

namespace RendererSystem {

/**
 * @brief Осевой прямоугольник: левый верхний угол и размер.
 */
struct Rect {
    glm::vec2 position{0.0f}; ///< Левый верхний угол.
    glm::vec2 size{0.0f};     ///< Ширина и высота.

    /// @brief Левый верхний угол.
    [[nodiscard]] glm::vec2 min() const noexcept { return position; }
    /// @brief Правый нижний угол.
    [[nodiscard]] glm::vec2 max() const noexcept { return position + size; }
    /// @brief Центр.
    [[nodiscard]] glm::vec2 center() const noexcept { return position + size * 0.5f; }

    /// @brief Точка внутри (правая и нижняя границы не включаются).
    [[nodiscard]] bool contains(glm::vec2 point) const noexcept {
        return point.x >= position.x && point.y >= position.y && point.x < position.x + size.x &&
               point.y < position.y + size.y;
    }

    /// @brief Прямоугольники пересекаются по площади.
    [[nodiscard]] bool intersects(const Rect& other) const noexcept {
        return position.x < other.position.x + other.size.x && other.position.x < position.x + size.x &&
               position.y < other.position.y + other.size.y && other.position.y < position.y + size.y;
    }

    bool operator==(const Rect&) const noexcept = default;
};

/**
 * @brief Область текстуры в UV-координатах (0..1), (0, 0) — левый верхний угол изображения.
 */
struct UvRect {
    glm::vec2 min{0.0f, 0.0f}; ///< Левый верхний угол.
    glm::vec2 max{1.0f, 1.0f}; ///< Правый нижний угол.

    /**
     * @brief UV-область по пиксельному прямоугольнику изображения.
     * @param pixels       Область в пикселях (например кадр спрайт-листа).
     * @param texture_size Размер текстуры в пикселях.
     */
    [[nodiscard]] static UvRect from_pixels(const Rect& pixels, glm::vec2 texture_size) noexcept {
        return UvRect{pixels.min() / texture_size, pixels.max() / texture_size};
    }

    /// @brief Размер области.
    [[nodiscard]] glm::vec2 size() const noexcept { return max - min; }

    bool operator==(const UvRect&) const noexcept = default;
};

} // namespace RendererSystem
