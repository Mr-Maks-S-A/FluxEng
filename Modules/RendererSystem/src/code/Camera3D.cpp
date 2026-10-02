#include <RendererSystem/Scene/Camera3D.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <cmath>

namespace RendererSystem {

Camera3D Camera3D::orbit(glm::vec3 target, float yaw, float pitch, float distance, glm::vec2 viewport) {
    const glm::vec3 offset{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    return Camera3D{.position = target + offset * distance, .target = target, .viewport = viewport};
}

glm::vec3 Camera3D::forward() const noexcept {
    return glm::normalize(target - position);
}

glm::vec3 Camera3D::right() const noexcept {
    return glm::normalize(glm::cross(forward(), up));
}

glm::mat4 Camera3D::view_matrix() const noexcept {
    return glm::lookAtRH(position, target, up);
}

glm::mat4 Camera3D::projection_matrix() const noexcept {
    const float aspect = viewport.y > 0.0f ? viewport.x / viewport.y : 1.0f;
    return glm::perspectiveRH_NO(fov_y, aspect, near_plane, far_plane);
}

glm::mat4 Camera3D::view_projection() const noexcept {
    return projection_matrix() * view_matrix();
}

Ray Camera3D::screen_to_ray(glm::vec2 screen) const noexcept {
    // Пиксель → NDC: X вправо, Y вверх (экран — Y вниз).
    const glm::vec2 ndc{screen.x / viewport.x * 2.0f - 1.0f, 1.0f - screen.y / viewport.y * 2.0f};
    const glm::mat4 inverse = glm::inverse(view_projection());
    glm::vec4 near_point = inverse * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 far_point = inverse * glm::vec4(ndc, 1.0f, 1.0f);
    near_point /= near_point.w;
    far_point /= far_point.w;
    return Ray{glm::vec3(near_point), glm::normalize(glm::vec3(far_point - near_point))};
}

ScreenPoint Camera3D::world_to_screen(glm::vec3 world) const noexcept {
    const glm::vec4 clip = view_projection() * glm::vec4(world, 1.0f);
    ScreenPoint out;
    if (clip.w <= 0.0f) {
        return out; // позади камеры
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    out.position = {(ndc.x + 1.0f) * 0.5f * viewport.x, (1.0f - ndc.y) * 0.5f * viewport.y};
    out.depth = ndc.z * 0.5f + 0.5f;
    out.visible = ndc.z >= -1.0f && ndc.z <= 1.0f;
    return out;
}

} // namespace RendererSystem
