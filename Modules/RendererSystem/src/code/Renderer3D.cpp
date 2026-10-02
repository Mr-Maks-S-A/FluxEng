#include <RendererSystem/Core/Error.hpp>
#include <RendererSystem/Renderer3D.hpp>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace RendererSystem {

namespace {

// Блоки uniform — одинаковые в обоих шейдерах (std140: vec4 и mat4 без дыр).
constexpr std::string_view uniform_blocks = R"(
const int MAX_POINTS = 8;

FLUX_UNIFORM(0, 0) Frame {
    mat4 view_projection;
    vec4 camera;
    vec4 ambient;
    vec4 sun_direction;
    vec4 sun_color;
    vec4 fog_color;
    vec4 fog_range;            // x — начало, y — конец, z — число точечных источников
    vec4 point_position[MAX_POINTS]; // xyz — позиция, w — радиус
    vec4 point_color[MAX_POINTS];    // цвет × яркость
} frame;

FLUX_UNIFORM(1, 0) Draw {
    mat4 model;
    mat4 normal_matrix;
    vec4 color;
    vec4 emissive;
    vec4 uv_rect;
    vec4 params;               // x — сила бликов, y — резкость, z — освещать (1/0)
} draw;
)";

constexpr std::string_view vertex_body = R"(
FLUX_LOCATION(0) in vec3 a_position;
FLUX_LOCATION(1) in vec3 a_normal;
FLUX_LOCATION(2) in vec2 a_uv;
FLUX_LOCATION(3) in vec4 a_color;

FLUX_VARYING(0) out vec3 v_world;
FLUX_VARYING(1) out vec3 v_normal;
FLUX_VARYING(2) out vec2 v_uv;
FLUX_VARYING(3) out vec4 v_color;

void main() {
    vec4 world = draw.model * vec4(a_position, 1.0);
    v_world = world.xyz;
    v_normal = mat3(draw.normal_matrix) * a_normal;
    v_uv = mix(draw.uv_rect.xy, draw.uv_rect.zw, a_uv);
    v_color = a_color;
    FLUX_POSITION(frame.view_projection * world);
}
)";

constexpr std::string_view fragment_body = R"(
FLUX_VARYING(0) in vec3 v_world;
FLUX_VARYING(1) in vec3 v_normal;
FLUX_VARYING(2) in vec2 v_uv;
FLUX_VARYING(3) in vec4 v_color;

FLUX_SAMPLER(2, 0) sampler2D u_texture;

FLUX_LOCATION(0) out vec4 frag_color;

void main() {
    vec4 base = texture(u_texture, v_uv) * draw.color * v_color;
    vec3 rgb = base.rgb;
    if (draw.params.z > 0.5) {
        vec3 n = normalize(v_normal);
        if (!gl_FrontFacing) n = -n;
        vec3 view = normalize(frame.camera.xyz - v_world);
        float shininess = max(draw.params.y, 1.0);

        vec3 l = normalize(-frame.sun_direction.xyz);
        float diffuse = max(dot(n, l), 0.0);
        vec3 light = frame.ambient.rgb + frame.sun_color.rgb * diffuse;
        vec3 highlight = diffuse > 0.0
            ? frame.sun_color.rgb * pow(max(dot(n, normalize(l + view)), 0.0), shininess) * draw.params.x
            : vec3(0.0);

        int points = int(frame.fog_range.z + 0.5);
        for (int i = 0; i < points; ++i) {
            vec3 to_light = frame.point_position[i].xyz - v_world;
            float distance = length(to_light);
            float falloff = clamp(1.0 - distance / frame.point_position[i].w, 0.0, 1.0);
            falloff *= falloff;
            vec3 pl = to_light / max(distance, 1e-4);
            float pd = max(dot(n, pl), 0.0);
            light += frame.point_color[i].rgb * pd * falloff;
            if (pd > 0.0) {
                highlight += frame.point_color[i].rgb * pow(max(dot(n, normalize(pl + view)), 0.0), shininess) * draw.params.x * falloff;
            }
        }
        rgb = rgb * light + highlight;
    }
    rgb += draw.emissive.rgb;
    if (frame.fog_range.y > frame.fog_range.x) {
        float fog = smoothstep(frame.fog_range.x, frame.fog_range.y, length(frame.camera.xyz - v_world));
        rgb = mix(rgb, frame.fog_color.rgb, fog);
    }
    frag_color = vec4(rgb, base.a);
}
)";

struct FrameUniforms {
    glm::mat4 view_projection{1.0f};
    glm::vec4 camera{0.0f};
    glm::vec4 ambient{0.0f};
    glm::vec4 sun_direction{0.0f};
    glm::vec4 sun_color{0.0f};
    glm::vec4 fog_color{0.0f};
    glm::vec4 fog_range{0.0f};
    std::array<glm::vec4, Environment::max_point_lights> point_position{};
    std::array<glm::vec4, Environment::max_point_lights> point_color{};
};
static_assert(sizeof(FrameUniforms) == 64 + 6 * 16 + 2 * 8 * 16, "FrameUniforms must match the std140 block");

struct DrawUniforms {
    glm::mat4 model{1.0f};
    glm::mat4 normal_matrix{1.0f};
    glm::vec4 color{1.0f};
    glm::vec4 emissive{0.0f};
    glm::vec4 uv_rect{0.0f, 0.0f, 1.0f, 1.0f};
    glm::vec4 params{0.0f};
};
static_assert(sizeof(DrawUniforms) == 192, "DrawUniforms must match the std140 block");

glm::vec4 rgb(Color color, float intensity = 1.0f) noexcept {
    return glm::vec4(glm::vec3(color.to_vec4()) * intensity, 0.0f);
}

std::size_t pipeline_index(BlendMode blend, bool double_sided) noexcept {
    return static_cast<std::size_t>(blend) * 2 + (double_sided ? 0 : 1);
}

} // namespace

std::expected<Renderer3D, std::string> Renderer3D::create(RHI::Device& device) {
    Renderer3D renderer(device);
    const std::string vertex = std::string(uniform_blocks) + std::string(vertex_body);
    const std::string fragment = std::string(uniform_blocks) + std::string(fragment_body);
    for (const BlendMode blend : {BlendMode::Opaque, BlendMode::Alpha, BlendMode::Additive}) {
        for (const bool double_sided : {true, false}) {
            auto pipeline = Pipeline::create(
                device, RHI::PipelineDesc{.name = "Renderer3D",
                                          .shader = {vertex, fragment},
                                          .layout = vertex3d_layout(),
                                          .blend = blend,
                                          .cull = double_sided ? RHI::CullMode::None : RHI::CullMode::Back,
                                          .depth = blend == BlendMode::Opaque ? RHI::DepthMode::TestWrite : RHI::DepthMode::Test});
            if (!pipeline) {
                return std::unexpected("Renderer3D: " + pipeline.error());
            }
            renderer.m_pipelines[pipeline_index(blend, double_sided)] = std::move(*pipeline);
        }
    }
    return renderer;
}

Renderer3D::Renderer3D(RHI::Device& device) : m_device(&device) {
    m_shapes[static_cast<std::size_t>(Shape::Cube)] = Mesh::create(device, MeshData::box());
    m_shapes[static_cast<std::size_t>(Shape::Sphere)] = Mesh::create(device, MeshData::sphere(0.5f, 32, 20));
    m_shapes[static_cast<std::size_t>(Shape::Quad)] = Mesh::create(device, MeshData::quad());
    m_shapes[static_cast<std::size_t>(Shape::Plane)] = Mesh::create(device, MeshData::plane());
    m_shapes[static_cast<std::size_t>(Shape::Cylinder)] = Mesh::create(device, MeshData::cylinder(0.5f, 1.0f, 32));
}

void Renderer3D::clear(Color color) {
    m_device->clear(color, true);
}

void Renderer3D::begin(const Camera3D& camera, const Environment& environment) {
    if (m_in_frame) {
        throw RendererError("Renderer3D::begin: previous frame was not finished with end()");
    }
    m_camera = camera;
    m_frustum = camera.frustum();
    m_environment = environment;
    m_opaque.clear();
    m_transparent.clear();
    m_stats = {};
    m_in_frame = true;
}

void Renderer3D::draw(const Mesh& mesh, const glm::mat4& model, const Material& material) {
    if (!m_in_frame) {
        throw RendererError("Renderer3D::draw: begin() was not called");
    }
    if (!mesh.valid() || mesh.vertex_count() == 0) {
        return;
    }
    Command command{.mesh = &mesh, .model = model, .material = material};
    const Aabb& local = mesh.bounds();
    glm::vec3 center = glm::vec3(model[3]);
    if (local != Aabb{}) {
        const Aabb world = local.transformed(model);
        if (!m_frustum.intersects(world)) {
            ++m_stats.culled;
            return;
        }
        center = world.center();
    }
    command.distance = glm::length(center - m_camera.position);
    if (material.blend == BlendMode::Opaque) {
        m_opaque.push_back(command);
    } else {
        m_transparent.push_back(command);
    }
}

RHI::PipelineId Renderer3D::pipeline_for(const Material& material) const noexcept {
    return m_pipelines[pipeline_index(material.blend, material.double_sided)].id();
}

void Renderer3D::submit(const Command& command, RHI::UniformSlice frame) {
    const Material& m = command.material;
    const DrawUniforms uniforms{.model = command.model,
                                .normal_matrix = glm::transpose(glm::inverse(command.model)),
                                .color = m.color.to_vec4(),
                                .emissive = glm::vec4(m.emissive, 0.0f),
                                .uv_rect = glm::vec4{m.uv.min, m.uv.max},
                                .params = glm::vec4{m.specular, m.shininess, m.lit ? 1.0f : 0.0f, 0.0f}};
    RHI::DrawCall call = command.mesh->draw_call(pipeline_for(m));
    call.texture = m.texture != nullptr ? m.texture->id() : RHI::TextureId{};
    call.frame = frame;
    call.draw_uniforms = std::as_bytes(std::span<const DrawUniforms>(&uniforms, 1));
    m_device->draw(call);
    ++m_stats.draws;
    m_stats.triangles += call.count / 3;
}

Render3DStats Renderer3D::end() {
    if (!m_in_frame) {
        throw RendererError("Renderer3D::end: begin() was not called");
    }
    m_in_frame = false;

    // Непрозрачные — группами по текстуре; прозрачные — от дальних к ближним (порядок смешивания важен).
    std::ranges::stable_sort(m_opaque, {}, [](const Command& c) { return c.material.texture; });
    std::ranges::stable_sort(m_transparent, std::ranges::greater{}, &Command::distance);

    FrameUniforms frame_uniforms{.view_projection = m_camera.view_projection(),
                                 .camera = glm::vec4(m_camera.position, 1.0f),
                                 .ambient = rgb(m_environment.ambient),
                                 .sun_direction = glm::vec4(glm::normalize(m_environment.sun.direction), 0.0f),
                                 .sun_color = rgb(m_environment.sun.color, m_environment.sun.intensity),
                                 .fog_color = rgb(m_environment.fog_color)};
    const std::size_t points = std::min(m_environment.point_count, Environment::max_point_lights);
    frame_uniforms.fog_range = glm::vec4{m_environment.fog_start, m_environment.fog_end, static_cast<float>(points), 0.0f};
    for (std::size_t i = 0; i < points; ++i) {
        const PointLight& p = m_environment.points[i];
        frame_uniforms.point_position[i] = glm::vec4{p.position, std::max(p.radius, 1e-3f)};
        frame_uniforms.point_color[i] = rgb(p.color, p.intensity);
    }
    const RHI::UniformSlice frame = m_device->push_uniform(frame_uniforms);

    const Texture* bound = nullptr;
    bool first = true;
    const auto count_bind = [&](const Material& material) {
        if (first || material.texture != bound) {
            bound = material.texture;
            first = false;
            ++m_stats.texture_binds;
        }
    };
    for (const Command& command : m_opaque) {
        count_bind(command.material);
        submit(command, frame);
    }
    for (const Command& command : m_transparent) {
        count_bind(command.material);
        submit(command, frame);
        ++m_stats.transparent;
    }
    return m_stats;
}

} // namespace RendererSystem
