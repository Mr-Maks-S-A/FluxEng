#pragma once
/**
 * @file Geometry3D.hpp
 * @brief Лучи, осевые коробки, плоскости и пирамида видимости — то, что нужно 3D-сцене без OpenGL.
 *
 * Соглашения 3D-части модуля: правая система координат, **ось Y вверх**, камера смотрит вдоль −Z
 * (как в `glm::lookAt`). Углы — в радианах.
 */

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

namespace RendererSystem {

/**
 * @brief Луч: точка и направление (единичной длины, если его строили функции модуля).
 */
struct Ray {
    glm::vec3 origin{0.0f};                   ///< Начало.
    glm::vec3 direction{0.0f, 0.0f, -1.0f};   ///< Направление.

    /// @brief Точка луча на параметре `t`.
    [[nodiscard]] glm::vec3 at(float t) const noexcept { return origin + direction * t; }

    /// @brief Луч в другой системе координат (например в локальной системе объекта: `inverse(model)`).
    [[nodiscard]] Ray transformed(const glm::mat4& matrix) const noexcept {
        return Ray{glm::vec3(matrix * glm::vec4(origin, 1.0f)), glm::vec3(matrix * glm::vec4(direction, 0.0f))};
    }
};

/**
 * @brief Осевая коробка (AABB). Нулевая — точка в начале координат (ZII).
 */
struct Aabb {
    glm::vec3 min{0.0f}; ///< Минимальный угол.
    glm::vec3 max{0.0f}; ///< Максимальный угол.

    /// @brief Коробка по центру и половине размера.
    [[nodiscard]] static Aabb from_center(glm::vec3 center, glm::vec3 half_extent) noexcept {
        return Aabb{center - half_extent, center + half_extent};
    }

    /// @brief «Пустая» коробка: любое expand() превращает её в точку.
    [[nodiscard]] static Aabb empty() noexcept {
        constexpr float big = std::numeric_limits<float>::max();
        return Aabb{glm::vec3{big}, glm::vec3{-big}};
    }

    [[nodiscard]] glm::vec3 center() const noexcept { return (min + max) * 0.5f; }  ///< Центр.
    [[nodiscard]] glm::vec3 size() const noexcept { return max - min; }             ///< Размер.
    [[nodiscard]] bool valid() const noexcept { return min.x <= max.x && min.y <= max.y && min.z <= max.z; } ///< Не пустая.

    /// @brief Точка внутри (границы включаются).
    [[nodiscard]] bool contains(glm::vec3 point) const noexcept {
        return point.x >= min.x && point.y >= min.y && point.z >= min.z && point.x <= max.x && point.y <= max.y &&
               point.z <= max.z;
    }

    /// @brief Коробки пересекаются (касание считается).
    [[nodiscard]] bool intersects(const Aabb& other) const noexcept {
        return min.x <= other.max.x && other.min.x <= max.x && min.y <= other.max.y && other.min.y <= max.y &&
               min.z <= other.max.z && other.min.z <= max.z;
    }

    /// @brief Расширяет коробку до точки.
    void expand(glm::vec3 point) noexcept {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    /// @brief Коробка, охватывающая эту после преобразования (8 углов).
    [[nodiscard]] Aabb transformed(const glm::mat4& matrix) const noexcept {
        Aabb out = empty();
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 p{(corner & 1) ? max.x : min.x, (corner & 2) ? max.y : min.y, (corner & 4) ? max.z : min.z};
            out.expand(glm::vec3(matrix * glm::vec4(p, 1.0f)));
        }
        return out;
    }

    bool operator==(const Aabb&) const noexcept = default;
};

/**
 * @brief Плоскость `dot(normal, p) + distance = 0`; нормаль единичная.
 */
struct Plane {
    glm::vec3 normal{0.0f, 1.0f, 0.0f}; ///< Нормаль (единичная).
    float distance = 0.0f;              ///< Смещение вдоль нормали со знаком минус.

    /// @brief Плоскость через точку с нормалью.
    [[nodiscard]] static Plane from_point_normal(glm::vec3 point, glm::vec3 normal) noexcept {
        const glm::vec3 n = glm::normalize(normal);
        return Plane{n, -glm::dot(n, point)};
    }

    /// @brief Расстояние со знаком: > 0 — по ту сторону, куда смотрит нормаль.
    [[nodiscard]] float signed_distance(glm::vec3 point) const noexcept { return glm::dot(normal, point) + distance; }
};

/**
 * @brief Пересечение луча с плоскостью.
 * @return Параметр `t >= 0` точки пересечения или пусто (параллельно / позади начала луча).
 */
[[nodiscard]] inline std::optional<float> intersect(const Ray& ray, const Plane& plane) noexcept {
    const float denominator = glm::dot(plane.normal, ray.direction);
    if (std::abs(denominator) < 1e-8f) {
        return std::nullopt;
    }
    const float t = -plane.signed_distance(ray.origin) / denominator;
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

/**
 * @brief Пересечение луча с коробкой (метод плит).
 * @return Параметр ближайшей точки входа (`0`, если начало луча внутри) или пусто.
 */
[[nodiscard]] inline std::optional<float> intersect(const Ray& ray, const Aabb& box) noexcept {
    float t_near = 0.0f;
    float t_far = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        const float origin = ray.origin[axis];
        const float direction = ray.direction[axis];
        if (std::abs(direction) < 1e-12f) {
            if (origin < box.min[axis] || origin > box.max[axis]) {
                return std::nullopt;
            }
            continue;
        }
        const float inverse = 1.0f / direction;
        float t0 = (box.min[axis] - origin) * inverse;
        float t1 = (box.max[axis] - origin) * inverse;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        t_near = std::max(t_near, t0);
        t_far = std::min(t_far, t1);
        if (t_near > t_far) {
            return std::nullopt;
        }
    }
    return t_near;
}

/**
 * @brief Пирамида видимости: шесть плоскостей из матрицы `projection * view` (метод Gribb–Hartmann).
 *
 * Нормали смотрят внутрь. Работает для любой проекции OpenGL (глубина −1…1).
 */
class Frustum {
public:
    /// @brief Плоскости в порядке: левая, правая, нижняя, верхняя, ближняя, дальняя.
    enum Side : int { Left = 0, Right, Bottom, Top, Near, Far };

    Frustum() = default;

    /// @brief Пирамида камеры с матрицей `view_projection`.
    explicit Frustum(const glm::mat4& view_projection) noexcept {
        const glm::mat4& m = view_projection;
        const glm::vec4 row0{m[0][0], m[1][0], m[2][0], m[3][0]};
        const glm::vec4 row1{m[0][1], m[1][1], m[2][1], m[3][1]};
        const glm::vec4 row2{m[0][2], m[1][2], m[2][2], m[3][2]};
        const glm::vec4 row3{m[0][3], m[1][3], m[2][3], m[3][3]};
        const std::array<glm::vec4, 6> raw{row3 + row0, row3 - row0, row3 + row1, row3 - row1, row3 + row2, row3 - row2};
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const glm::vec3 normal{raw[i]};
            const float length = glm::length(normal);
            m_planes[i] = Plane{normal / length, raw[i].w / length};
        }
    }

    /// @brief Плоскость стороны.
    [[nodiscard]] const Plane& plane(Side side) const noexcept { return m_planes[static_cast<std::size_t>(side)]; }

    /// @brief Точка внутри пирамиды.
    [[nodiscard]] bool contains(glm::vec3 point) const noexcept {
        return std::ranges::all_of(m_planes, [&](const Plane& p) { return p.signed_distance(point) >= 0.0f; });
    }

    /// @brief Сфера хотя бы частично внутри.
    [[nodiscard]] bool intersects_sphere(glm::vec3 center, float radius) const noexcept {
        return std::ranges::all_of(m_planes, [&](const Plane& p) { return p.signed_distance(center) >= -radius; });
    }

    /// @brief Коробка хотя бы частично внутри (консервативно: у углов пирамиды возможны ложные «видна»).
    [[nodiscard]] bool intersects(const Aabb& box) const noexcept {
        for (const Plane& p : m_planes) {
            const glm::vec3 farthest{p.normal.x > 0 ? box.max.x : box.min.x, p.normal.y > 0 ? box.max.y : box.min.y,
                                     p.normal.z > 0 ? box.max.z : box.min.z};
            if (p.signed_distance(farthest) < 0.0f) {
                return false;
            }
        }
        return true;
    }

private:
    std::array<Plane, 6> m_planes{};
};

} // namespace RendererSystem
