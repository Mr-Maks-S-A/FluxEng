#pragma once
/**
 * @file Camera3D.hpp
 * @brief Перспективная 3D-камера: матрицы, луч из пикселя экрана и проекция точки мира на экран.
 */

#include <RendererSystem/Core/Geometry3D.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <numbers>

namespace RendererSystem {

/**
 * @brief Точка мира на экране.
 */
struct ScreenPoint {
    glm::vec2 position{0.0f}; ///< Пиксели, (0, 0) — левый верх (как у Camera2D и курсора).
    float depth = 0.0f;       ///< Глубина 0 (ближняя плоскость) … 1 (дальняя).
    bool visible = false;     ///< Точка перед камерой и внутри экрана по глубине.
};

/**
 * @brief Камера «смотрит из `position` на `target`».
 *
 * Мир: Y вверх, правая система (см. Geometry3D.hpp). `viewport` — размер области вывода в пикселях;
 * от него зависят соотношение сторон и пересчёт экран ↔ мир.
 *
 * @code
 * Camera3D camera{.position = {0, 10, 8}, .target = {0, 0, 0}, .viewport = {1280, 720}};
 * Ray ray = camera.screen_to_ray(mouse);              // выбор объекта мышью
 * ScreenPoint label = camera.world_to_screen(head);   // подпись над головой в 2D-оверлее
 * @endcode
 */
struct Camera3D {
    glm::vec3 position{0.0f, 0.0f, 5.0f};             ///< Глаз.
    glm::vec3 target{0.0f};                            ///< Точка, на которую смотрит камера.
    glm::vec3 up{0.0f, 1.0f, 0.0f};                    ///< «Верх» мира.
    float fov_y = std::numbers::pi_v<float> / 3.0f;    ///< Вертикальный угол обзора, радианы.
    float near_plane = 0.1f;                           ///< Ближняя плоскость отсечения.
    float far_plane = 200.0f;                          ///< Дальняя плоскость отсечения.
    glm::vec2 viewport{1.0f, 1.0f};                    ///< Размер области вывода, пиксели.

    /// @brief Камера на сфере вокруг `target`: `yaw` — вокруг Y, `pitch` — вверх от горизонта.
    [[nodiscard]] static Camera3D orbit(glm::vec3 target, float yaw, float pitch, float distance, glm::vec2 viewport);

    /// @brief Направление взгляда (единичное).
    [[nodiscard]] glm::vec3 forward() const noexcept;
    /// @brief Вправо от взгляда (единичное).
    [[nodiscard]] glm::vec3 right() const noexcept;

    /// @brief Матрица вида (мир → камера).
    [[nodiscard]] glm::mat4 view_matrix() const noexcept;
    /// @brief Перспективная проекция (камера → clip space, глубина OpenGL −1…1).
    [[nodiscard]] glm::mat4 projection_matrix() const noexcept;
    /// @brief `projection * view`.
    [[nodiscard]] glm::mat4 view_projection() const noexcept;
    /// @brief Пирамида видимости камеры.
    [[nodiscard]] Frustum frustum() const noexcept { return Frustum(view_projection()); }

    /// @brief Луч из пикселя экрана (0,0 — левый верх) в мир; начало — на ближней плоскости.
    [[nodiscard]] Ray screen_to_ray(glm::vec2 screen) const noexcept;
    /// @brief Точка мира → пиксель экрана.
    [[nodiscard]] ScreenPoint world_to_screen(glm::vec3 world) const noexcept;
};

} // namespace RendererSystem
