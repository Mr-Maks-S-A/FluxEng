#include <RendererSystem/Mesh/MeshData.hpp>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace RendererSystem {

namespace {

constexpr float pi = std::numbers::pi_v<float>;

std::uint32_t next_index(const MeshData& mesh) noexcept {
    return static_cast<std::uint32_t>(mesh.vertices.size());
}

/// Прямоугольник с центром `center`, полуосями `right` и `up` (right × up — нормаль), UV (0,0) — левый верх.
void add_quad(MeshData& mesh, glm::vec3 center, glm::vec3 right, glm::vec3 up) {
    const glm::vec3 normal = glm::normalize(glm::cross(right, up));
    const std::uint32_t base = next_index(mesh);
    mesh.vertices.push_back({center - right + up, normal, {0.0f, 0.0f}}); // левый верх
    mesh.vertices.push_back({center + right + up, normal, {1.0f, 0.0f}}); // правый верх
    mesh.vertices.push_back({center + right - up, normal, {1.0f, 1.0f}}); // правый низ
    mesh.vertices.push_back({center - right - up, normal, {0.0f, 1.0f}}); // левый низ
    mesh.indices.insert(mesh.indices.end(), {base + 0, base + 3, base + 2, base + 0, base + 2, base + 1});
}

/// Точка контура скруглённого прямоугольника: позиция и нормаль наружу (в плоскости XY).
struct OutlinePoint {
    glm::vec2 position;
    glm::vec2 normal;
};

/// Контур против часовой стрелки (если смотреть с +Z), начиная с правого верхнего угла.
std::vector<OutlinePoint> rounded_outline(glm::vec2 size, float radius, int corner_segments) {
    const glm::vec2 half = size * 0.5f;
    const float r = std::clamp(radius, 0.0f, std::min(half.x, half.y));
    const int steps = std::max(corner_segments, 1);
    const glm::vec2 centers[4] = {{half.x - r, half.y - r}, {-half.x + r, half.y - r},
                                  {-half.x + r, -half.y + r}, {half.x - r, -half.y + r}};
    std::vector<OutlinePoint> outline;
    outline.reserve(static_cast<std::size_t>(4 * (steps + 1)));
    for (int corner = 0; corner < 4; ++corner) {
        for (int step = 0; step <= steps; ++step) {
            const float angle = (static_cast<float>(corner) + static_cast<float>(step) / static_cast<float>(steps)) * pi * 0.5f;
            const glm::vec2 direction{std::cos(angle), std::sin(angle)};
            outline.push_back({centers[corner] + direction * r, direction});
        }
    }
    return outline;
}

glm::vec2 outline_uv(glm::vec2 position, glm::vec2 half, bool mirrored) noexcept {
    const float u = mirrored ? (half.x - position.x) / (2.0f * half.x) : (position.x + half.x) / (2.0f * half.x);
    return {u, (half.y - position.y) / (2.0f * half.y)};
}

/// Веер треугольников из центра по контуру. `front` — лицом к +Z, иначе к −Z (обход обратный, UV отражены).
void add_outline_fan(MeshData& mesh, const std::vector<OutlinePoint>& outline, glm::vec2 half, float z, bool front) {
    const glm::vec3 normal{0.0f, 0.0f, front ? 1.0f : -1.0f};
    const std::uint32_t center = next_index(mesh);
    mesh.vertices.push_back({{0.0f, 0.0f, z}, normal, {0.5f, 0.5f}});
    for (const OutlinePoint& p : outline) {
        mesh.vertices.push_back({{p.position, z}, normal, outline_uv(p.position, half, !front)});
    }
    const auto count = static_cast<std::uint32_t>(outline.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t a = center + 1 + i;
        const std::uint32_t b = center + 1 + (i + 1) % count;
        if (front) {
            mesh.indices.insert(mesh.indices.end(), {center, a, b});
        } else {
            mesh.indices.insert(mesh.indices.end(), {center, b, a});
        }
    }
}

} // namespace

Aabb MeshData::bounds() const noexcept {
    if (vertices.empty()) {
        return Aabb{};
    }
    Aabb box = Aabb::empty();
    for (const Vertex3D& v : vertices) {
        box.expand(v.position);
    }
    return box;
}

void MeshData::clear() noexcept {
    vertices.clear();
    indices.clear();
}

void MeshData::append(const MeshData& other, const glm::mat4& transform) {
    const std::uint32_t base = next_index(*this);
    const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(transform)));
    vertices.reserve(vertices.size() + other.vertices.size());
    for (Vertex3D v : other.vertices) {
        v.position = glm::vec3(transform * glm::vec4(v.position, 1.0f));
        v.normal = glm::normalize(normal_matrix * v.normal);
        vertices.push_back(v);
    }
    indices.reserve(indices.size() + other.indices.size());
    for (const std::uint32_t index : other.indices) {
        indices.push_back(base + index);
    }
}

void MeshData::set_color(Color color) noexcept {
    for (Vertex3D& v : vertices) {
        v.color = color;
    }
}

void MeshData::compute_normals() {
    for (Vertex3D& v : vertices) {
        v.normal = glm::vec3{0.0f};
    }
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        Vertex3D& a = vertices[indices[i]];
        Vertex3D& b = vertices[indices[i + 1]];
        Vertex3D& c = vertices[indices[i + 2]];
        const glm::vec3 face = glm::cross(b.position - a.position, c.position - a.position); // длина ~ площадь
        a.normal += face;
        b.normal += face;
        c.normal += face;
    }
    for (Vertex3D& v : vertices) {
        const float length = glm::length(v.normal);
        v.normal = length > 0.0f ? v.normal / length : glm::vec3{0.0f, 1.0f, 0.0f};
    }
}

MeshData MeshData::box(glm::vec3 size) {
    const glm::vec3 h = size * 0.5f;
    MeshData mesh;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);
    const glm::vec3 x{h.x, 0.0f, 0.0f};
    const glm::vec3 y{0.0f, h.y, 0.0f};
    const glm::vec3 z{0.0f, 0.0f, h.z};
    add_quad(mesh, z, x, y);    // +Z
    add_quad(mesh, -z, -x, y);  // −Z
    add_quad(mesh, x, -z, y);   // +X
    add_quad(mesh, -x, z, y);   // −X
    add_quad(mesh, y, x, -z);   // +Y: верх картинки — дальний край (−Z)
    add_quad(mesh, -y, x, z);   // −Y
    return mesh;
}

MeshData MeshData::quad(glm::vec2 size) {
    MeshData mesh;
    add_quad(mesh, glm::vec3{0.0f}, {size.x * 0.5f, 0.0f, 0.0f}, {0.0f, size.y * 0.5f, 0.0f});
    return mesh;
}

MeshData MeshData::plane(glm::vec2 size, glm::ivec2 segments) {
    const int sx = std::max(segments.x, 1);
    const int sz = std::max(segments.y, 1);
    MeshData mesh;
    mesh.vertices.reserve(static_cast<std::size_t>((sx + 1) * (sz + 1)));
    for (int row = 0; row <= sz; ++row) {
        for (int column = 0; column <= sx; ++column) {
            const glm::vec2 uv{static_cast<float>(column) / static_cast<float>(sx), static_cast<float>(row) / static_cast<float>(sz)};
            mesh.vertices.push_back({{(uv.x - 0.5f) * size.x, 0.0f, (uv.y - 0.5f) * size.y}, {0.0f, 1.0f, 0.0f}, uv});
        }
    }
    const auto stride = static_cast<std::uint32_t>(sx + 1);
    for (std::uint32_t row = 0; row < static_cast<std::uint32_t>(sz); ++row) {
        for (std::uint32_t column = 0; column < static_cast<std::uint32_t>(sx); ++column) {
            const std::uint32_t top_left = row * stride + column;
            const std::uint32_t bottom_left = top_left + stride;
            mesh.indices.insert(mesh.indices.end(),
                                {top_left, bottom_left, bottom_left + 1, top_left, bottom_left + 1, top_left + 1});
        }
    }
    return mesh;
}

MeshData MeshData::sphere(float radius, int segments, int rings) {
    const int seg = std::max(segments, 3);
    const int ring = std::max(rings, 2);
    MeshData mesh;
    mesh.vertices.reserve(static_cast<std::size_t>((seg + 1) * (ring + 1)));
    for (int r = 0; r <= ring; ++r) {
        const float phi = pi * static_cast<float>(r) / static_cast<float>(ring);
        for (int s = 0; s <= seg; ++s) {
            const float theta = 2.0f * pi * static_cast<float>(s) / static_cast<float>(seg);
            const glm::vec3 normal{std::sin(phi) * std::sin(theta), std::cos(phi), std::sin(phi) * std::cos(theta)};
            mesh.vertices.push_back({normal * radius, normal,
                                     {static_cast<float>(s) / static_cast<float>(seg), static_cast<float>(r) / static_cast<float>(ring)}});
        }
    }
    const auto stride = static_cast<std::uint32_t>(seg + 1);
    for (std::uint32_t r = 0; r < static_cast<std::uint32_t>(ring); ++r) {
        for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(seg); ++s) {
            const std::uint32_t a = r * stride + s;
            const std::uint32_t b = a + stride;
            mesh.indices.insert(mesh.indices.end(), {a, b, b + 1, a, b + 1, a + 1});
        }
    }
    return mesh;
}

MeshData MeshData::cylinder(float radius, float height, int segments) {
    const int seg = std::max(segments, 3);
    const float h = height * 0.5f;
    MeshData mesh;

    // Боковая поверхность.
    for (int s = 0; s <= seg; ++s) {
        const float theta = 2.0f * pi * static_cast<float>(s) / static_cast<float>(seg);
        const glm::vec3 normal{std::sin(theta), 0.0f, std::cos(theta)};
        const float u = static_cast<float>(s) / static_cast<float>(seg);
        mesh.vertices.push_back({normal * radius + glm::vec3{0.0f, h, 0.0f}, normal, {u, 0.0f}});
        mesh.vertices.push_back({normal * radius - glm::vec3{0.0f, h, 0.0f}, normal, {u, 1.0f}});
    }
    for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(seg); ++s) {
        const std::uint32_t top = s * 2;
        const std::uint32_t bottom = top + 1;
        mesh.indices.insert(mesh.indices.end(), {top, bottom, bottom + 2, top, bottom + 2, top + 2});
    }

    // Крышки: веер из центра, UV — вид сверху (верх картинки — дальний край, −Z).
    for (const float side : {1.0f, -1.0f}) {
        const glm::vec3 normal{0.0f, side, 0.0f};
        const std::uint32_t center = next_index(mesh);
        mesh.vertices.push_back({{0.0f, h * side, 0.0f}, normal, {0.5f, 0.5f}});
        for (int s = 0; s < seg; ++s) {
            const float theta = 2.0f * pi * static_cast<float>(s) / static_cast<float>(seg);
            const glm::vec2 p{std::sin(theta), std::cos(theta)};
            mesh.vertices.push_back({{p.x * radius, h * side, p.y * radius}, normal, {p.x * 0.5f + 0.5f, p.y * 0.5f + 0.5f}});
        }
        for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(seg); ++s) {
            const std::uint32_t a = center + 1 + s;
            const std::uint32_t b = center + 1 + (s + 1) % static_cast<std::uint32_t>(seg);
            if (side > 0.0f) {
                mesh.indices.insert(mesh.indices.end(), {center, a, b});
            } else {
                mesh.indices.insert(mesh.indices.end(), {center, b, a});
            }
        }
    }
    return mesh;
}

MeshData MeshData::rounded_rect(glm::vec2 size, float radius, int corner_segments) {
    MeshData mesh;
    add_outline_fan(mesh, rounded_outline(size, radius, corner_segments), size * 0.5f, 0.0f, true);
    return mesh;
}

MeshData MeshData::rounded_slab(glm::vec2 size, float thickness, float radius, int corner_segments) {
    const std::vector<OutlinePoint> outline = rounded_outline(size, radius, corner_segments);
    const glm::vec2 half = size * 0.5f;
    const float hz = thickness * 0.5f;
    MeshData mesh;
    add_outline_fan(mesh, outline, half, hz, true);
    add_outline_fan(mesh, outline, half, -hz, false);

    // Бортик: по паре вершин на точку контура, нормаль — наружу от центра скругления.
    const std::uint32_t base = next_index(mesh);
    const auto count = static_cast<std::uint32_t>(outline.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        const OutlinePoint& p = outline[i];
        const glm::vec3 normal{p.normal, 0.0f};
        const float u = static_cast<float>(i) / static_cast<float>(count);
        mesh.vertices.push_back({{p.position, hz}, normal, {u, 0.0f}});
        mesh.vertices.push_back({{p.position, -hz}, normal, {u, 1.0f}});
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t a = base + i * 2;              // лицевой край, точка i
        const std::uint32_t d = a + 1;                     // оборотный край, точка i
        const std::uint32_t b = base + ((i + 1) % count) * 2; // лицевой край, точка i+1
        const std::uint32_t c = b + 1;
        mesh.indices.insert(mesh.indices.end(), {a, d, c, a, c, b});
    }
    return mesh;
}

} // namespace RendererSystem
