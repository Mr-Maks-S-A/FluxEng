#include <WorldRender/FogView.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace WorldRender {

using namespace RendererSystem;

namespace {

constexpr RHI::VertexLayout fog_layout =
    RHI::VertexLayout::make(sizeof(FogVertex), {{0, 3, RHI::AttributeType::Float, offsetof(FogVertex, center)},
                                                {1, 2, RHI::AttributeType::Float, offsetof(FogVertex, corner)},
                                                {2, 1, RHI::AttributeType::Float, offsetof(FogVertex, density)}});

constexpr std::string_view uniforms = R"(
FLUX_UNIFORM(0, 0) Frame {
    mat4 view_projection;
    vec4 right;
    vec4 up;
    vec4 params;   // x — размер, y — яркость, z — дальность
    vec4 shift;    // смещение глаза с момента построения вершин
    vec4 color;
} frame;
)";

constexpr std::string_view vertex_shader = R"(
FLUX_LOCATION(0) in vec3 a_center;
FLUX_LOCATION(1) in vec2 a_corner;
FLUX_LOCATION(2) in float a_density;
FLUX_VARYING(0) out vec2 v_corner;
FLUX_VARYING(1) out float v_density;
void main() {
    vec3 centre = a_center + frame.shift.xyz;
    vec3 position = centre + (frame.right.xyz * a_corner.x + frame.up.xyz * a_corner.y) * frame.params.x * 0.5;
    v_corner = a_corner;
    // Билборды гаснут вблизи камеры (иначе заслоняют персонажа) и у границы дальности (иначе «выскакивают»).
    float distance = length(centre);
    v_density = a_density * a_density *   // квадрат плотности: провал от заклинания контрастнее
         smoothstep(1.0, 4.0, distance) * (1.0 - smoothstep(frame.params.z * 0.6, frame.params.z, distance));
    FLUX_POSITION(frame.view_projection * vec4(position, 1.0));
}
)";

// Мягкий диск: яркость ∝ плотности. Additive: итог = цвет · α + фон.
constexpr std::string_view fragment_shader = R"(
FLUX_VARYING(0) in vec2 v_corner;
FLUX_VARYING(1) in float v_density;
FLUX_LOCATION(0) out vec4 frag_color;
void main() {
    float falloff = 1.0 - smoothstep(0.0, 1.0, length(v_corner));
    float a = frame.params.y * v_density * falloff;
    frag_color = vec4(frame.color.rgb, a);
}
)";

struct FrameUniforms {
    glm::mat4 view_projection;
    glm::vec4 right, up, params, shift, color;
};


} // namespace

std::size_t build_fog_vertices(const FogSource& source, const glm::dvec3& eye, float radius, std::vector<FogVertex>& out) {
    out.clear();
    const double cell = source.cell_meters();
    const CellBox box = source.bounds();
    if (cell <= 0.0 || box.hi[0] < box.lo[0] || box.hi[1] < box.lo[1] || box.hi[2] < box.lo[2]) return 0;
    const auto lo = [&](double e, int axis) { return std::max(box.lo[axis], static_cast<std::int64_t>(std::floor((e - radius) / cell))); };
    const auto hi = [&](double e, int axis) { return std::min(box.hi[axis], static_cast<std::int64_t>(std::floor((e + radius) / cell))); };
    const double r2 = static_cast<double>(radius) * radius;
    std::size_t cells = 0;
    for (std::int64_t y = lo(eye.y, 1); y <= hi(eye.y, 1); ++y) {
        for (std::int64_t z = lo(eye.z, 2); z <= hi(eye.z, 2); ++z) {
            for (std::int64_t x = lo(eye.x, 0); x <= hi(eye.x, 0); ++x) {
                const glm::dvec3 centre{(static_cast<double>(x) + 0.5) * cell, (static_cast<double>(y) + 0.5) * cell, (static_cast<double>(z) + 0.5) * cell};
                const glm::dvec3 d = centre - eye;
                if (d.x * d.x + d.y * d.y + d.z * d.z > r2) continue;
                const float density = source.density(x, y, z);
                if (density < 0.04f) continue; // почти пусто — не рисуем (источник возвращает 0 для скрытых ячеек)
                const glm::vec3 c = glm::vec3(d);
                for (const auto& [cx, cy] : {std::pair{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}) {
                    out.push_back({{c.x, c.y, c.z}, {cx, cy}, density});
                }
                ++cells;
            }
        }
    }
    return cells;
}

FogView::FogView(RHI::Device& device) : m_device(&device) {
    const std::string vs = std::string(uniforms) + std::string(vertex_shader), fs = std::string(uniforms) + std::string(fragment_shader);
    auto pipeline = Pipeline::create(device, {.name = "mana fog", .shader = {vs, fs}, .layout = fog_layout, .primitive = RHI::Primitive::Triangles,
                                              .blend = RHI::BlendMode::Additive, .cull = RHI::CullMode::None, .depth = RHI::DepthMode::Test});
    if (!pipeline) throw std::runtime_error("fog pipeline: " + pipeline.error());
    m_pipeline = std::move(*pipeline);
    m_mesh = Mesh::create(device, fog_layout, RHI::BufferUsage::Stream);
}

void FogView::draw(const FogSource& source, const View& view) {
    m_cells = build_fog_vertices(source, view.eye, radius, m_vertices);
    if (m_cells == 0) return;
    if (m_indices.size() < m_cells * 6) {
        const std::size_t old = m_indices.size() / 6;
        m_indices.resize(m_cells * 6);
        for (std::size_t q = old; q < m_cells; ++q) {
            const auto b = static_cast<std::uint32_t>(q * 4);
            const std::uint32_t quad[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
            std::copy(quad, quad + 6, m_indices.begin() + static_cast<std::ptrdiff_t>(q * 6));
        }
    }
    m_mesh.upload(std::span<const FogVertex>(m_vertices), std::span<const std::uint32_t>(m_indices.data(), m_cells * 6));
    const FrameUniforms frame_data{view.camera.view_projection(), glm::vec4(view.right, 0.0f), glm::vec4(view.up, 0.0f), {size, intensity, radius, 0.0f},
                                   {0.0f, 0.0f, 0.0f, 0.0f}, {0.45f, 0.55f, 1.0f, 1.0f}};
    RHI::DrawCall call = m_mesh.draw_call(m_pipeline.id());
    call.frame = m_device->push_uniform(frame_data);
    m_device->draw(call);
}

} // namespace WorldRender
