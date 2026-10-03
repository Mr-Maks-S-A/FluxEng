#include <WorldRender/TerrainView.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>

namespace WorldRender {

using namespace RendererSystem;

namespace {

constexpr RHI::VertexLayout terrain_layout =
    RHI::VertexLayout::make(sizeof(SurfaceVertex), {{0, 3, RHI::AttributeType::Float, offsetof(SurfaceVertex, position)},
                                                    {1, 3, RHI::AttributeType::Float, offsetof(SurfaceVertex, normal)},
                                                    {2, 4, RHI::AttributeType::UnsignedByteNorm, offsetof(SurfaceVertex, material)}});

constexpr std::string_view uniforms = R"(
FLUX_UNIFORM(0, 0) Frame {
    mat4 view_projection;
    vec4 sun_direction;  // куда светит
    vec4 sun_color;
    vec4 ambient;
    vec4 fog_color;
    vec4 fog_range;      // x — начало, y — конец
} frame;

FLUX_UNIFORM(1, 0) Draw {
    vec4 offset;         // xyz — угол чанка относительно глаза, w — высота угла в мире
} draw;
)";

constexpr std::string_view vertex_shader = R"(
FLUX_LOCATION(0) in vec3 a_position;
FLUX_LOCATION(1) in vec3 a_normal;
FLUX_LOCATION(2) in vec4 a_material;
FLUX_VARYING(0) out vec3 v_normal;
FLUX_VARYING(1) out float v_height;
FLUX_VARYING(2) out float v_distance;
void main() {
    vec3 relative = draw.offset.xyz + a_position;
    v_normal = a_normal;
    v_height = draw.offset.w + a_position.y;
    v_distance = length(relative);
    FLUX_POSITION(frame.view_projection * vec4(relative, 1.0));
}
)";

// Цвет по высоте и уклону: песок у воды, трава на пологом, камень на крутом, снег наверху. Текстур нет.
constexpr std::string_view fragment_shader = R"(
FLUX_VARYING(0) in vec3 v_normal;
FLUX_VARYING(1) in float v_height;
FLUX_VARYING(2) in float v_distance;
FLUX_LOCATION(0) out vec4 frag_color;
void main() {
    vec3 n = normalize(v_normal);
    float flat_ness = smoothstep(0.62, 0.86, n.y);
    vec3 rock = vec3(0.46, 0.43, 0.41);
    vec3 grass = vec3(0.30, 0.52, 0.22);
    vec3 sand = vec3(0.76, 0.70, 0.50);
    vec3 snow = vec3(0.94, 0.95, 0.98);
    vec3 c = mix(rock, grass, flat_ness);
    c = mix(c, sand, (1.0 - smoothstep(14.0, 17.0, v_height)) * smoothstep(0.45, 0.8, n.y));
    c = mix(c, snow, smoothstep(33.0, 36.0, v_height) * smoothstep(0.45, 0.75, n.y));
    float diffuse = max(dot(n, -normalize(frame.sun_direction.xyz)), 0.0);
    vec3 lit = c * (frame.ambient.rgb + frame.sun_color.rgb * diffuse);
    float fog = smoothstep(frame.fog_range.x, frame.fog_range.y, v_distance);
    frag_color = vec4(mix(lit, frame.fog_color.rgb, fog), 1.0);
}
)";

struct FrameUniforms {
    glm::mat4 view_projection;
    glm::vec4 sun_direction, sun_color, ambient, fog_color, fog_range;
};

struct DrawUniforms {
    glm::vec4 offset;
};

Pipeline make_pipeline(RHI::Device& device, RHI::Primitive primitive) {
    const std::string vs = std::string(uniforms) + std::string(vertex_shader), fs = std::string(uniforms) + std::string(fragment_shader);
    auto pipeline = Pipeline::create(device, {.name = "terrain", .shader = {vs, fs}, .layout = terrain_layout, .primitive = primitive,
                                              .cull = primitive == RHI::Primitive::Triangles ? RHI::CullMode::Back : RHI::CullMode::None,
                                              .depth = RHI::DepthMode::TestWrite});
    if (!pipeline) throw std::runtime_error("terrain pipeline: " + pipeline.error());
    return std::move(*pipeline);
}

} // namespace

// ------------------------------------------------------------------ MeshQueue

void MeshQueue::push(ChunkIndex c) {
    const auto i = static_cast<std::size_t>(m_shape.index(c));
    if (m_queued[i]) return;
    m_queued[i] = 1;
    m_queue.push_back(c);
}

void MeshQueue::push(std::span<const ChunkIndex> chunks) {
    for (const ChunkIndex c : chunks) push(c);
}

std::vector<ChunkIndex> MeshQueue::pop_nearest(std::size_t count, const glm::dvec3& eye) {
    const auto distance_sq = [&](ChunkIndex c) {
        const glm::dvec3 d = m_shape.chunk_origin(c) + glm::dvec3(m_shape.chunk_meters * 0.5) - eye;
        return d.x * d.x + d.y * d.y + d.z * d.z;
    };
    count = std::min(count, m_queue.size());
    std::ranges::partial_sort(m_queue, m_queue.begin() + static_cast<std::ptrdiff_t>(count),
                              [&](ChunkIndex a, ChunkIndex b) { return distance_sq(a) < distance_sq(b); });
    std::vector<ChunkIndex> out(m_queue.begin(), m_queue.begin() + static_cast<std::ptrdiff_t>(count));
    m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<std::ptrdiff_t>(count));
    for (const ChunkIndex c : out) m_queued[static_cast<std::size_t>(m_shape.index(c))] = 0;
    return out;
}

// ---------------------------------------------------------------- TerrainView

TerrainView::TerrainView(RHI::Device& device, const SurfaceSource& source)
    : m_device(&device),
      m_solid(make_pipeline(device, RHI::Primitive::Triangles)),
      m_lines(make_pipeline(device, RHI::Primitive::Lines)),
      m_source(&source),
      m_shape(source.shape()),
      m_slots(static_cast<std::size_t>(m_shape.chunk_count())),
      m_queue(m_shape) {}

void TerrainView::update(JobSystem::Scheduler& jobs, const glm::dvec3& eye, std::size_t budget) {
    const auto start = std::chrono::steady_clock::now();
    const std::vector<ChunkIndex> coords = m_queue.pop_nearest(budget == 0 ? std::numeric_limits<std::size_t>::max() : budget, eye);
    std::vector<SurfaceMesh> meshes(coords.size());
    // Сетки строятся параллельно: источник только читает мир, результат пишет каждая задача в свою ячейку.
    JobSystem::parallel_for(jobs, coords.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) m_source->build(coords[i], meshes[i]);
    });
    for (std::size_t i = 0; i < coords.size(); ++i) {
        Slot& slot = m_slots[static_cast<std::size_t>(m_shape.index(coords[i]))];
        slot.cpu = std::move(meshes[i]);
        slot.has_mesh = !slot.cpu.vertices.empty();
        slot.wire_ready = false;
        if (!slot.has_mesh) continue;
        if (!slot.solid.valid()) slot.solid = Mesh::create(*m_device, terrain_layout, RHI::BufferUsage::Dynamic);
        slot.solid.upload(std::span<const SurfaceVertex>(slot.cpu.vertices), std::span<const std::uint32_t>(slot.cpu.indices));
    }
    m_stats.built = static_cast<std::uint32_t>(coords.size());
    m_stats.queued = m_queue.size();
    if (!coords.empty()) {
        m_stats.triangles = 0;
        m_stats.gpu_bytes = 0;
        for (const Slot& s : m_slots) {
            m_stats.triangles += s.cpu.triangle_count();
            m_stats.gpu_bytes += s.solid.gpu_bytes() + s.wire.gpu_bytes();
        }
    }
    m_stats.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void TerrainView::draw(const View& view, const Light& light) {
    const FrameUniforms frame_data{
        .view_projection = view.camera.view_projection(),
        .sun_direction = glm::vec4(light.direction, 0.0f),
        .sun_color = glm::vec4(light.color, 1.0f),
        .ambient = glm::vec4(light.ambient, 1.0f),
        .fog_color = glm::vec4(light.fog_color, 1.0f),
        .fog_range = {light.fog_start, light.fog_end, 0.0f, 0.0f}};
    const RHI::UniformSlice frame = m_device->push_uniform(frame_data);
    const Frustum frustum = view.camera.frustum();
    m_stats.drawn = m_stats.culled = 0;
    for (int i = 0; i < m_shape.chunk_count(); ++i) {
        Slot& slot = m_slots[static_cast<std::size_t>(i)];
        if (!slot.has_mesh) continue;
        const glm::dvec3 origin = m_shape.chunk_origin(m_shape.coord(i));
        const glm::vec3 relative = view.relative(origin);
        if (!frustum.intersects(Aabb{relative, relative + glm::vec3(static_cast<float>(m_shape.chunk_meters))})) {
            ++m_stats.culled;
            continue;
        }
        const DrawUniforms draw_data{glm::vec4(relative, static_cast<float>(origin.y))};
        if (m_wireframe) {
            if (!slot.wire_ready) { // индексы линий строятся по требованию: по три ребра на треугольник
                std::vector<std::uint32_t> lines;
                lines.reserve(slot.cpu.indices.size() * 2);
                for (std::size_t t = 0; t + 2 < slot.cpu.indices.size(); t += 3) {
                    const auto a = slot.cpu.indices[t], b = slot.cpu.indices[t + 1], d = slot.cpu.indices[t + 2];
                    lines.insert(lines.end(), {a, b, b, d, d, a});
                }
                if (!slot.wire.valid()) slot.wire = Mesh::create(*m_device, terrain_layout, RHI::BufferUsage::Dynamic);
                slot.wire.upload(std::span<const SurfaceVertex>(slot.cpu.vertices), std::span<const std::uint32_t>(lines));
                slot.wire_ready = true;
            }
            RHI::DrawCall call = slot.wire.draw_call(m_lines.id());
            call.frame = frame;
            call.draw_uniforms = std::as_bytes(std::span(&draw_data, 1));
            m_device->draw(call);
        } else {
            RHI::DrawCall call = slot.solid.draw_call(m_solid.id());
            call.frame = frame;
            call.draw_uniforms = std::as_bytes(std::span(&draw_data, 1));
            m_device->draw(call);
        }
        ++m_stats.drawn;
    }
}

void TerrainView::add_chunk_boxes(DebugDraw& lines, const GridShape& shape, Color color) {
    for (int i = 0; i < shape.chunk_count(); ++i) {
        const glm::dvec3 origin = shape.chunk_origin(shape.coord(i));
        lines.box(origin, origin + glm::dvec3(shape.chunk_meters), color);
    }
}

} // namespace WorldRender
