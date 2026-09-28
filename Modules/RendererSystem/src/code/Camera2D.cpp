#include <RendererSystem/Scene/Camera2D.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace RendererSystem {

namespace {

glm::vec2 rotate(glm::vec2 point, float angle) noexcept {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {point.x * c - point.y * s, point.x * s + point.y * c};
}

} // namespace

glm::mat4 Camera2D::view_matrix() const noexcept {
    glm::mat4 view = glm::rotate(glm::mat4(1.0f), -rotation, glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::translate(view, glm::vec3(-position, 0.0f));
}

glm::mat4 Camera2D::projection_matrix() const noexcept {
    const glm::vec2 half = viewport * 0.5f / zoom;
    // bottom = +half.y, top = -half.y: ось Y мира направлена вниз, как на экране.
    return glm::ortho(-half.x, half.x, half.y, -half.y, -1.0f, 1.0f);
}

glm::mat4 Camera2D::view_projection() const noexcept {
    return projection_matrix() * view_matrix();
}

glm::vec2 Camera2D::screen_to_world(glm::vec2 screen) const noexcept {
    return rotate((screen - viewport * 0.5f) / zoom, rotation) + position;
}

glm::vec2 Camera2D::world_to_screen(glm::vec2 world) const noexcept {
    return rotate(world - position, -rotation) * zoom + viewport * 0.5f;
}

} // namespace RendererSystem
