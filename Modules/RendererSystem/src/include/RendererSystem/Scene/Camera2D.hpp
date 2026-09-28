#pragma once
/**
 * @file Camera2D.hpp
 * @brief Ортографическая 2D-камера: матрицы и перевод координат экран ↔ мир.
 */

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

namespace RendererSystem {

/**
 * @brief 2D-камера с осью Y вниз.
 *
 * `position` — точка мира в центре экрана. `zoom` > 1 приближает.
 * `rotation` — поворот камеры в радианах. `viewport` — размер области вывода в пикселях.
 *
 * При `zoom == 1` одна единица мира равна одному пикселю экрана.
 *
 * @code
 * Camera2D camera{.position = {0, 0}, .zoom = 2.0f, .viewport = {1280, 720}};
 * glm::vec2 world = camera.screen_to_world(mouse_position);
 * @endcode
 */
struct Camera2D {
    glm::vec2 position{0.0f};          ///< Точка мира в центре экрана.
    float zoom = 1.0f;                 ///< Масштаб (> 0).
    float rotation = 0.0f;             ///< Поворот камеры, радианы.
    glm::vec2 viewport{1.0f, 1.0f};    ///< Размер области вывода, пиксели.

    /// @brief Матрица вида (мир → пространство камеры).
    [[nodiscard]] glm::mat4 view_matrix() const noexcept;
    /// @brief Ортографическая проекция (камера → clip space), ось Y вниз.
    [[nodiscard]] glm::mat4 projection_matrix() const noexcept;
    /// @brief `projection * view` — то, что передаётся в Renderer2D::begin().
    [[nodiscard]] glm::mat4 view_projection() const noexcept;

    /// @brief Пиксель экрана (0,0 — левый верхний угол) → точка мира.
    [[nodiscard]] glm::vec2 screen_to_world(glm::vec2 screen) const noexcept;
    /// @brief Точка мира → пиксель экрана.
    [[nodiscard]] glm::vec2 world_to_screen(glm::vec2 world) const noexcept;
};

} // namespace RendererSystem
