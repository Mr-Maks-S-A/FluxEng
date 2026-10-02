#include <RendererSystem/Core/Error.hpp>
#include <RendererSystem/RHI/Resources.hpp>

#include <format>
#include <utility>

namespace RendererSystem {

// =============================================================================
// Texture
// =============================================================================

Texture Texture::create(RHI::Device& device, const Image& image, const TextureDesc& desc) {
    if (image.empty()) {
        throw RendererError("Texture: image is empty");
    }
    Texture texture;
    texture.m_device = &device;
    texture.m_id = device.create_texture(image.width(), image.height(), desc, image.pixels());
    texture.m_width = image.width();
    texture.m_height = image.height();
    texture.m_desc = desc;
    return texture;
}

Texture Texture::create_empty(RHI::Device& device, int width, int height, const TextureDesc& desc) {
    if (width <= 0 || height <= 0) {
        throw RendererError(std::format("Texture: invalid size {}x{}", width, height));
    }
    Texture texture;
    texture.m_device = &device;
    texture.m_id = device.create_texture(width, height, desc, {});
    texture.m_width = width;
    texture.m_height = height;
    texture.m_desc = desc;
    return texture;
}

Texture::~Texture() {
    release();
}

Texture::Texture(Texture&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_id(std::exchange(other.m_id, {})),
      m_width(std::exchange(other.m_width, 0)),
      m_height(std::exchange(other.m_height, 0)),
      m_desc(other.m_desc),
      m_owned(other.m_owned) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_id = std::exchange(other.m_id, {});
        m_width = std::exchange(other.m_width, 0);
        m_height = std::exchange(other.m_height, 0);
        m_desc = other.m_desc;
        m_owned = other.m_owned;
    }
    return *this;
}

void Texture::release() noexcept {
    if (m_device != nullptr && m_id.valid() && m_owned) {
        m_device->destroy_texture(m_id);
    }
    m_id = {};
}

void Texture::update(const Image& image) {
    if (image.width() != m_width || image.height() != m_height) {
        throw RendererError(std::format("Texture::update: size {}x{} does not match texture {}x{}", image.width(),
                                        image.height(), m_width, m_height));
    }
    if (m_device != nullptr) m_device->update_texture(m_id, image.pixels());
}

void Texture::generate_mipmaps() const {
    if (m_device != nullptr && m_desc.mipmaps) m_device->generate_mipmaps(m_id);
}

// =============================================================================
// RenderTarget
// =============================================================================

std::expected<RenderTarget, std::string> RenderTarget::create(RHI::Device& device, int width, int height, const TargetDesc& desc) {
    if (width <= 0 || height <= 0) {
        return std::unexpected(std::format("invalid render target size {}x{}", width, height));
    }
    auto id = device.create_target(width, height, desc);
    if (!id) {
        return std::unexpected(id.error());
    }
    RenderTarget target;
    target.m_device = &device;
    target.m_id = *id;
    target.m_depth = desc.depth;
    target.m_color.m_device = &device;
    target.m_color.m_id = device.target_texture(*id);
    target.m_color.m_width = width;
    target.m_color.m_height = height;
    target.m_color.m_desc = desc.color;
    target.m_color.m_owned = false;
    return target;
}

RenderTarget::~RenderTarget() {
    release();
}

RenderTarget::RenderTarget(RenderTarget&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_id(std::exchange(other.m_id, {})),
      m_color(std::move(other.m_color)),
      m_depth(other.m_depth) {}

RenderTarget& RenderTarget::operator=(RenderTarget&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_id = std::exchange(other.m_id, {});
        m_color = std::move(other.m_color);
        m_depth = other.m_depth;
    }
    return *this;
}

void RenderTarget::release() noexcept {
    if (m_device != nullptr && m_id.valid()) {
        m_device->destroy_target(m_id); // вместе с цветовой текстурой (m_color не владеет)
    }
    m_id = {};
}

void RenderTarget::bind() const {
    if (m_device != nullptr) m_device->bind_target(m_id);
}

void RenderTarget::bind_screen(RHI::Device& device) {
    device.bind_target({});
}

UvRect RenderTarget::uv() const noexcept {
    return m_device != nullptr ? m_device->target_uv() : UvRect{};
}

Image RenderTarget::read_pixels() const {
    if (m_device == nullptr) throw RendererError("RenderTarget::read_pixels: empty target");
    return m_device->read_target(m_id);
}

// =============================================================================
// Mesh
// =============================================================================

Mesh Mesh::create(RHI::Device& device, const RHI::VertexLayout& layout, RHI::BufferUsage usage) {
    if (layout.stride == 0 || layout.attribute_count == 0) {
        throw RendererError("Mesh: vertex layout is empty");
    }
    Mesh mesh;
    mesh.m_device = &device;
    mesh.m_layout = layout;
    mesh.m_vertices = device.create_buffer(RHI::BufferKind::Vertex, usage);
    mesh.m_indices = device.create_buffer(RHI::BufferKind::Index, usage);
    return mesh;
}

Mesh Mesh::create(RHI::Device& device, const MeshData& data, RHI::BufferUsage usage) {
    Mesh mesh = create(device, vertex3d_layout(), usage);
    mesh.upload(data);
    return mesh;
}

Mesh::~Mesh() {
    release();
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_layout(other.m_layout),
      m_vertices(std::exchange(other.m_vertices, {})),
      m_indices(std::exchange(other.m_indices, {})),
      m_vertex_count(std::exchange(other.m_vertex_count, 0)),
      m_index_count(std::exchange(other.m_index_count, 0)),
      m_bounds(other.m_bounds) {}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_layout = other.m_layout;
        m_vertices = std::exchange(other.m_vertices, {});
        m_indices = std::exchange(other.m_indices, {});
        m_vertex_count = std::exchange(other.m_vertex_count, 0);
        m_index_count = std::exchange(other.m_index_count, 0);
        m_bounds = other.m_bounds;
    }
    return *this;
}

void Mesh::release() noexcept {
    if (m_device != nullptr) {
        if (m_vertices.valid()) m_device->destroy_buffer(m_vertices);
        if (m_indices.valid()) m_device->destroy_buffer(m_indices);
    }
    m_vertices = m_indices = {};
    m_vertex_count = m_index_count = 0;
}

void Mesh::upload_bytes(std::span<const std::byte> vertices, std::span<const std::uint32_t> indices) {
    if (!valid()) {
        throw RendererError("Mesh::upload: mesh was not created");
    }
    if (vertices.size() % m_layout.stride != 0) {
        throw RendererError(std::format("Mesh::upload: {} bytes is not a multiple of vertex stride {}", vertices.size(),
                                        m_layout.stride));
    }
    m_device->update_buffer(m_vertices, vertices);
    m_device->update_buffer(m_indices, std::as_bytes(indices));
    m_vertex_count = vertices.size() / m_layout.stride;
    m_index_count = indices.size();
}

void Mesh::upload(const MeshData& data) {
    if (m_layout.stride != sizeof(Vertex3D)) {
        throw RendererError("Mesh::upload(MeshData): mesh layout is not Vertex3D");
    }
    upload_bytes(std::as_bytes(std::span(data.vertices)), data.indices);
    m_bounds = data.bounds();
}

RHI::DrawCall Mesh::draw_call(RHI::PipelineId pipeline) const noexcept {
    RHI::DrawCall call{.pipeline = pipeline, .vertices = m_vertices};
    if (m_index_count > 0) {
        call.indices = m_indices;
        call.count = static_cast<std::uint32_t>(m_index_count);
    } else {
        call.count = static_cast<std::uint32_t>(m_vertex_count);
    }
    return call;
}

// =============================================================================
// Pipeline
// =============================================================================

std::expected<Pipeline, std::string> Pipeline::create(RHI::Device& device, const RHI::PipelineDesc& desc) {
    auto id = device.create_pipeline(desc);
    if (!id) {
        return std::unexpected(id.error());
    }
    Pipeline pipeline;
    pipeline.m_device = &device;
    pipeline.m_id = *id;
    return pipeline;
}

Pipeline::~Pipeline() {
    release();
}

Pipeline::Pipeline(Pipeline&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_id(std::exchange(other.m_id, {})) {}

Pipeline& Pipeline::operator=(Pipeline&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_id = std::exchange(other.m_id, {});
    }
    return *this;
}

void Pipeline::release() noexcept {
    if (m_device != nullptr && m_id.valid()) m_device->destroy_pipeline(m_id);
    m_id = {};
}

} // namespace RendererSystem
