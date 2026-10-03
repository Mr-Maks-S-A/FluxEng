#pragma once
/**
 * @file Camera.hpp
 * @brief Две камеры (свободная и следящая от третьего лица) и отрисовка относительно камеры.
 *
 * Положение камеры — `glm::dvec3` в метрах мира. В GPU уходят только разности «точка − глаз» (`View::relative`):
 * вершины ландшафта лежат от угла своего чанка, а сам чанк смещён на разность двойной точности, поэтому float
 * не теряет точность вдали от начала координат. `View::camera` — обычная Camera3D с глазом в нуле.
 */

#include <RendererSystem/Scene/Camera3D.hpp>

#include <Math/Sdf.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace WorldRender {

/// @brief Вид кадра: глаз в мире (double) и камера, у которой глаз в нуле.
struct View {
    glm::dvec3 eye{0.0};
    RendererSystem::Camera3D camera{};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};

    /// @brief Мировая точка → вектор от глаза (для float-шейдеров).
    [[nodiscard]] glm::vec3 relative(const glm::dvec3& world) const noexcept { return glm::vec3(world - eye); }
};

/// @brief Направление взгляда по рысканью и тангажу: yaw вокруг Y (0 — вдоль +X), pitch вверх от горизонта.
[[nodiscard]] glm::vec3 look_direction(float yaw, float pitch) noexcept;

/// @brief Камера-рига: переключается между следящей (по умолчанию) и свободной.
class CameraRig {
public:
    enum class Mode : std::uint8_t { Follow, Free };

    float yaw = 0.0f;
    float pitch = -0.35f;
    float distance = 5.5f;    ///< Следящая: расстояние до персонажа.
    float fov_y = 1.1f;
    float free_speed = 18.0f; ///< Свободная: м/с.

    [[nodiscard]] Mode mode() const noexcept { return m_mode; }
    [[nodiscard]] bool is_free() const noexcept { return m_mode == Mode::Free; }
    /// @brief Переключает режим; при уходе в свободный режим камера остаётся там, где была.
    void toggle();

    /// @brief Обзор мышью (пиксели курсора за кадр).
    void look(float dx, float dy, float sensitivity = 0.0025f) noexcept;
    /// @brief Свободная камера: движение в осях взгляда (forward, right, up — от −1 до 1).
    void fly(float forward, float right, float up, float seconds) noexcept;

    /// @brief Вид кадра. `target` — точка, за которой следят (голова персонажа); `ground` — любая поверхность (`Math::SdfField`: ландшафт, плоскость, планета), чтобы камера не ушла в неё.
    [[nodiscard]] View view(const glm::dvec3& target, const Math::SdfField& ground, glm::vec2 viewport);
    [[nodiscard]] glm::vec3 forward() const noexcept { return look_direction(yaw, pitch); }
    [[nodiscard]] glm::dvec3 position() const noexcept { return m_position; }

private:
    Mode m_mode = Mode::Follow;
    glm::dvec3 m_position{0.0};
};

} // namespace WorldRender
