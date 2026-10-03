#include <WorldRender/Camera.hpp>

#include <Math/Vec.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace WorldRender {

glm::vec3 look_direction(float yaw, float pitch) noexcept {
    return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
}

void CameraRig::toggle() { m_mode = m_mode == Mode::Follow ? Mode::Free : Mode::Follow; }

void CameraRig::look(float dx, float dy, float sensitivity) noexcept {
    yaw += dx * sensitivity;
    pitch = std::clamp(pitch - dy * sensitivity, -1.5f, 1.5f);
}

void CameraRig::fly(float forward_axis, float right_axis, float up_axis, float seconds) noexcept {
    const glm::vec3 f = forward();
    const glm::vec3 r = glm::normalize(glm::cross(f, glm::vec3{0.0f, 1.0f, 0.0f}));
    const glm::vec3 move = f * forward_axis + r * right_axis + glm::vec3{0.0f, up_axis, 0.0f};
    m_position += glm::dvec3(move) * static_cast<double>(free_speed * seconds);
}


View CameraRig::view(const glm::dvec3& target, const Math::SdfField& ground, glm::vec2 viewport) {
    const glm::vec3 f = forward();
    if (m_mode == Mode::Follow) {
        float d = distance;
        const Math::FVec3 back = Math::quantize_direction(-f.x, -f.y, -f.z);
        if (const auto hit = Math::raycast(ground, Math::WorldPos::from_doubles(target.x, target.y, target.z), back, Math::Fixed::from_double(distance))) {
            d = std::max(0.6f, static_cast<float>(hit->distance.to_double()) - 0.5f); // не заходить в породу
        }
        m_position = target - glm::dvec3(f) * static_cast<double>(d);
    }
    View v;
    v.eye = m_position;
    v.camera = RendererSystem::Camera3D{.position = {0.0f, 0.0f, 0.0f}, .target = f, .fov_y = fov_y, .near_plane = 0.1f, .far_plane = 500.0f, .viewport = viewport};
    v.right = v.camera.right();
    v.up = glm::normalize(glm::cross(v.right, f));
    return v;
}

} // namespace WorldRender
