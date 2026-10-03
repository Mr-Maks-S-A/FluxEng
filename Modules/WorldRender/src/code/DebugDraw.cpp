#include <WorldRender/DebugDraw.hpp>

#include <stdexcept>
#include <string>

namespace WorldRender {

using namespace RendererSystem;

namespace {

constexpr RHI::VertexLayout line_layout = RHI::VertexLayout::make(
    sizeof(LineVertex), {{0, 3, RHI::AttributeType::Float, offsetof(LineVertex, position)}, {1, 4, RHI::AttributeType::UnsignedByteNorm, offsetof(LineVertex, rgba)}});

constexpr std::string_view vertex_shader = R"(
FLUX_UNIFORM(0, 0) Frame {
    mat4 view_projection;
} frame;
FLUX_LOCATION(0) in vec3 a_position;
FLUX_LOCATION(1) in vec4 a_color;
FLUX_VARYING(0) out vec4 v_color;
void main() {
    v_color = a_color;
    FLUX_POSITION(frame.view_projection * vec4(a_position, 1.0));
}
)";

constexpr std::string_view fragment_shader = R"(
FLUX_UNIFORM(0, 0) Frame {
    mat4 view_projection;
} frame;
FLUX_VARYING(0) in vec4 v_color;
FLUX_LOCATION(0) out vec4 frag_color;
void main() { frag_color = v_color; }
)";

std::uint32_t pack(Color c) { return static_cast<std::uint32_t>(c.r) | (static_cast<std::uint32_t>(c.g) << 8) | (static_cast<std::uint32_t>(c.b) << 16) | (static_cast<std::uint32_t>(c.a) << 24); }

} // namespace

void DebugDraw::line(const glm::dvec3& a, const glm::dvec3& b, Color color) {
    const std::uint32_t packed = pack(color);
    m_lines.emplace_back(a, packed);
    m_lines.emplace_back(b, packed);
}

void DebugDraw::box(const glm::dvec3& lo, const glm::dvec3& hi, Color color) {
    const auto corner = [&](int i) { return glm::dvec3{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z}; };
    for (int i = 0; i < 8; ++i) {
        for (const int bit : {1, 2, 4}) {
            if (!(i & bit)) line(corner(i), corner(i | bit), color);
        }
    }
}

void DebugDraw::cross(const glm::dvec3& at, double h, Color color) {
    line(at - glm::dvec3{h, 0, 0}, at + glm::dvec3{h, 0, 0}, color);
    line(at - glm::dvec3{0, h, 0}, at + glm::dvec3{0, h, 0}, color);
    line(at - glm::dvec3{0, 0, h}, at + glm::dvec3{0, 0, h}, color);
}

LineRenderer::LineRenderer(RHI::Device& device) : m_device(&device) {
    auto pipeline = Pipeline::create(device, {.name = "debug lines", .shader = {std::string(vertex_shader), std::string(fragment_shader)}, .layout = line_layout,
                                              .primitive = RHI::Primitive::Lines, .cull = RHI::CullMode::None, .depth = RHI::DepthMode::Test});
    if (!pipeline) throw std::runtime_error("debug lines pipeline: " + pipeline.error());
    m_pipeline = std::move(*pipeline);
    m_mesh = Mesh::create(device, line_layout, RHI::BufferUsage::Stream);
}

void LineRenderer::flush(DebugDraw& lines, const View& view) {
    if (lines.empty()) return;
    m_scratch.clear();
    for (const auto& [position, rgba] : lines.raw()) {
        const glm::vec3 p = view.relative(position);
        LineVertex v{{p.x, p.y, p.z}, {}};
        for (int i = 0; i < 4; ++i) v.rgba[i] = static_cast<std::uint8_t>(rgba >> (8 * i));
        m_scratch.push_back(v);
    }
    m_mesh.upload(std::span<const LineVertex>(m_scratch));
    RHI::DrawCall call = m_mesh.draw_call(m_pipeline.id());
    call.frame = m_device->push_uniform(view.camera.view_projection());
    m_device->draw(call);
    lines.clear();
}

} // namespace WorldRender
