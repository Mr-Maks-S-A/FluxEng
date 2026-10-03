#include <RendererSystem/Core/Error.hpp>
#include <RendererSystem/Renderer2D.hpp>


#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <utility>

namespace RendererSystem {

namespace {

constexpr std::string_view vertex_shader_source = R"(
FLUX_LOCATION(0) in vec2 a_position;
FLUX_LOCATION(1) in vec2 a_uv;
FLUX_LOCATION(2) in vec4 a_color;

FLUX_UNIFORM(0, 0) Frame {
    mat4 view_projection;
} frame;

FLUX_VARYING(0) out vec2 v_uv;
FLUX_VARYING(1) out vec4 v_color;

void main() {
    v_uv = a_uv;
    v_color = a_color;
    FLUX_POSITION(frame.view_projection * vec4(a_position, 0.0, 1.0));
}
)";

constexpr std::string_view fragment_shader_source = R"(
FLUX_VARYING(0) in vec2 v_uv;
FLUX_VARYING(1) in vec4 v_color;

FLUX_SAMPLER(2, 0) sampler2D u_texture;

FLUX_LOCATION(0) out vec4 frag_color;

void main() {
    frag_color = texture(u_texture, v_uv) * v_color;
}
)";

constexpr RHI::VertexLayout sprite_layout =
    RHI::VertexLayout::make(sizeof(SpriteVertex), {{0, 2, RHI::AttributeType::Float, offsetof(SpriteVertex, position)},
                                                   {1, 2, RHI::AttributeType::Float, offsetof(SpriteVertex, uv)},
                                                   {2, 4, RHI::AttributeType::UnsignedByteNorm, offsetof(SpriteVertex, color)}});

} // namespace

std::expected<Renderer2D, std::string> Renderer2D::create(RHI::Device& device, const RendererConfig& config) {
    auto pipeline = Pipeline::create(device, RHI::PipelineDesc{.name = "Renderer2D",
                                                               .shader = {vertex_shader_source, fragment_shader_source},
                                                               .layout = sprite_layout,
                                                               .blend = RHI::BlendMode::Alpha});
    if (!pipeline) {
        return std::unexpected("Renderer2D: " + pipeline.error());
    }
    return Renderer2D(device, std::move(*pipeline), config);
}

Renderer2D::Renderer2D(RHI::Device& device, Pipeline pipeline, const RendererConfig& config)
    : m_device(&device), m_pipeline(std::move(pipeline)), m_batch(config.sort_mode) {
    m_vertices = device.create_buffer(RHI::BufferKind::Vertex, RHI::BufferUsage::Stream);
    m_indices = device.create_buffer(RHI::BufferKind::Index, RHI::BufferUsage::Static);
    ensure_capacity(std::max<std::size_t>(config.initial_quad_capacity, 1));

    // Дескриптор 0 — белая текстура 1×1 для заливок и линий.
    m_textures.push_back(Texture::create(device, Image(1, 1, Colors::white)));
}

Renderer2D::~Renderer2D() {
    release();
}

Renderer2D::Renderer2D(Renderer2D&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_pipeline(std::move(other.m_pipeline)),
      m_textures(std::move(other.m_textures)),
      m_fonts(std::move(other.m_fonts)),
      m_glyphs(std::move(other.m_glyphs)),
      m_batch(std::move(other.m_batch)),
      m_view_projection(other.m_view_projection),
      m_stats(other.m_stats),
      m_vertices(std::exchange(other.m_vertices, {})),
      m_indices(std::exchange(other.m_indices, {})),
      m_quad_capacity(std::exchange(other.m_quad_capacity, 0)),
      m_in_frame(std::exchange(other.m_in_frame, false)) {}

Renderer2D& Renderer2D::operator=(Renderer2D&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_pipeline = std::move(other.m_pipeline);
        m_textures = std::move(other.m_textures);
        m_fonts = std::move(other.m_fonts);
        m_glyphs = std::move(other.m_glyphs);
        m_batch = std::move(other.m_batch);
        m_view_projection = other.m_view_projection;
        m_stats = other.m_stats;
        m_vertices = std::exchange(other.m_vertices, {});
        m_indices = std::exchange(other.m_indices, {});
        m_quad_capacity = std::exchange(other.m_quad_capacity, 0);
        m_in_frame = std::exchange(other.m_in_frame, false);
    }
    return *this;
}

void Renderer2D::release() noexcept {
    if (m_device != nullptr) {
        if (m_vertices.valid()) m_device->destroy_buffer(m_vertices);
        if (m_indices.valid()) m_device->destroy_buffer(m_indices);
    }
    m_vertices = m_indices = {};
    m_quad_capacity = 0;
}

void Renderer2D::ensure_capacity(std::size_t quads) {
    if (quads <= m_quad_capacity) {
        return;
    }
    const std::size_t capacity = std::max(quads, m_quad_capacity * 2);
    std::vector<std::uint32_t> indices(capacity * 6);
    for (std::size_t quad = 0; quad < capacity; ++quad) {
        const auto base = static_cast<std::uint32_t>(quad * 4);
        std::uint32_t* out = &indices[quad * 6];
        out[0] = base + 0;
        out[1] = base + 1;
        out[2] = base + 2;
        out[3] = base + 2;
        out[4] = base + 3;
        out[5] = base + 0;
    }
    m_device->update_buffer(m_indices, std::as_bytes(std::span(indices)));
    m_quad_capacity = capacity;
}

TextureHandle Renderer2D::create_texture(const Image& image, const TextureDesc& desc) {
    m_textures.push_back(Texture::create(*m_device, image, desc));
    return TextureHandle{static_cast<std::uint32_t>(m_textures.size() - 1)};
}

std::expected<TextureHandle, std::string> Renderer2D::load_texture(const std::filesystem::path& path,
                                                                   const TextureDesc& desc) {
    auto image = Image::load(path);
    if (!image) {
        return std::unexpected(image.error());
    }
    return create_texture(*image, desc);
}

void Renderer2D::update_texture(TextureHandle handle, const Image& image) {
    if (handle.index >= m_textures.size()) {
        throw RendererError(std::format("Renderer2D: unknown texture handle {}", handle.index));
    }
    m_textures[handle.index].update(image);
}

const Texture& Renderer2D::texture(TextureHandle handle) const {
    if (handle.index >= m_textures.size()) {
        throw RendererError(std::format("Renderer2D: unknown texture handle {}", handle.index));
    }
    return m_textures[handle.index];
}

FontHandle Renderer2D::add_font(Font font, const TextureDesc& desc) {
    const TextureHandle texture = create_texture(font.atlas(), desc);
    m_fonts.push_back(FontEntry{std::move(font), texture});
    return FontHandle{static_cast<std::uint32_t>(m_fonts.size() - 1)};
}

const Font& Renderer2D::font(FontHandle handle) const {
    if (handle.index >= m_fonts.size()) {
        throw RendererError(std::format("Renderer2D: unknown font handle {}", handle.index));
    }
    return m_fonts[handle.index].font;
}

glm::vec2 Renderer2D::measure_text(FontHandle handle, std::string_view text, const TextStyle& style) const {
    return font(handle).measure(text, TextLayoutOptions{.size = style.size, .align = style.align,
                                                        .max_width = style.max_width, .line_spacing = style.line_spacing});
}

glm::vec2 Renderer2D::draw_text(FontHandle handle, std::string_view text, glm::vec2 top_left, const TextStyle& style) {
    const Font& f = font(handle);
    const TextureHandle texture = m_fonts[handle.index].texture;
    m_glyphs.clear();
    const glm::vec2 size = f.layout(text, TextLayoutOptions{.size = style.size, .align = style.align,
                                                            .max_width = style.max_width, .line_spacing = style.line_spacing},
                                    m_glyphs);
    const auto submit = [&](glm::vec2 offset, Color color) {
        for (const GlyphQuad& quad : m_glyphs) {
            m_batch.submit(SpriteInstance{.position = top_left + offset + quad.rect.position, .size = quad.rect.size,
                                          .pivot = {0.0f, 0.0f}, .uv = quad.uv, .color = color, .texture = texture,
                                          .layer = style.layer});
        }
    };
    if (style.shadow.a > 0) {
        submit(style.shadow_offset, style.shadow); // поразрядная сортировка устойчива: тень остаётся под текстом
    }
    submit(glm::vec2{0.0f}, style.color);
    return size;
}

void Renderer2D::set_viewport(int width, int height) {
    m_device->set_viewport(width, height);
}

void Renderer2D::clear(Color color) {
    m_device->clear(color, false);
}

void Renderer2D::begin(const glm::mat4& view_projection) {
    if (m_in_frame) {
        throw RendererError("Renderer2D::begin: previous frame was not finished with end()");
    }
    m_batch.clear();
    m_view_projection = view_projection;
    m_in_frame = true;
}

RenderStats Renderer2D::end() {
    if (!m_in_frame) {
        throw RendererError("Renderer2D::end: begin() was not called");
    }
    m_in_frame = false;

    m_batch.build();
    const auto commands = m_batch.commands();
    for (const DrawCommand& command : commands) {
        if (command.texture.index >= m_textures.size()) {
            throw RendererError(std::format("Renderer2D: unknown texture handle {}", command.texture.index));
        }
    }

    m_stats = RenderStats{.quads = static_cast<std::uint32_t>(m_batch.quad_count())};
    if (m_batch.quad_count() == 0) {
        return m_stats;
    }

    ensure_capacity(m_batch.quad_count());
    m_device->update_buffer(m_vertices, std::as_bytes(m_batch.vertices()));
    const RHI::UniformSlice frame = m_device->push_uniform(m_view_projection);

    TextureHandle bound{std::numeric_limits<std::uint32_t>::max()}; // «ничего не привязано»
    for (const DrawCommand& command : commands) {
        if (command.texture != bound) {
            bound = command.texture;
            ++m_stats.texture_binds;
        }
        m_device->draw(RHI::DrawCall{.pipeline = m_pipeline.id(), .vertices = m_vertices, .indices = m_indices,
                                     .first = command.first_quad * 6, .count = command.quad_count * 6,
                                     .texture = m_textures[command.texture.index].id(), .frame = frame});
        ++m_stats.draw_calls;
    }
    return m_stats;
}

} // namespace RendererSystem
