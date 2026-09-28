#include <RendererSystem/Core/Error.hpp>
#include <RendererSystem/Renderer2D.hpp>

#include <glad/glad.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <utility>

namespace RendererSystem {

namespace {

constexpr std::string_view vertex_shader_source = R"(#version 330 core
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

uniform mat4 u_view_projection;

out vec2 v_uv;
out vec4 v_color;

void main() {
    gl_Position = u_view_projection * vec4(a_position, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
}
)";

constexpr std::string_view fragment_shader_source = R"(#version 330 core
in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_texture;

out vec4 frag_color;

void main() {
    frag_color = texture(u_texture, v_uv) * v_color;
}
)";

const void* byte_offset(std::size_t bytes) noexcept {
    return reinterpret_cast<const void*>(bytes); // NOLINT(performance-no-int-to-ptr): так OpenGL задаёт смещения
}

} // namespace

std::expected<Renderer2D, std::string> Renderer2D::create(const RendererConfig& config) {
    auto shader = GL::Shader::from_source(vertex_shader_source, fragment_shader_source);
    if (!shader) {
        return std::unexpected("Renderer2D: " + shader.error());
    }
    return Renderer2D(std::move(*shader), config);
}

Renderer2D::Renderer2D(GL::Shader shader, const RendererConfig& config)
    : m_shader(std::move(shader)), m_batch(config.sort_mode) {
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    constexpr auto stride = static_cast<GLsizei>(sizeof(SpriteVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, byte_offset(offsetof(SpriteVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, byte_offset(offsetof(SpriteVertex, uv)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, byte_offset(offsetof(SpriteVertex, color)));
    glBindVertexArray(0);

    ensure_capacity(std::max<std::size_t>(config.initial_quad_capacity, 1));

    // Дескриптор 0 — белая текстура 1×1 для заливок и линий.
    m_textures.push_back(GL::Texture::create(Image(1, 1, Colors::white)));
}

Renderer2D::~Renderer2D() {
    release();
}

Renderer2D::Renderer2D(Renderer2D&& other) noexcept
    : m_shader(std::move(other.m_shader)),
      m_textures(std::move(other.m_textures)),
      m_batch(std::move(other.m_batch)),
      m_view_projection(other.m_view_projection),
      m_stats(other.m_stats),
      m_vao(std::exchange(other.m_vao, 0)),
      m_vbo(std::exchange(other.m_vbo, 0)),
      m_ebo(std::exchange(other.m_ebo, 0)),
      m_quad_capacity(std::exchange(other.m_quad_capacity, 0)),
      m_in_frame(std::exchange(other.m_in_frame, false)) {}

Renderer2D& Renderer2D::operator=(Renderer2D&& other) noexcept {
    if (this != &other) {
        release();
        m_shader = std::move(other.m_shader);
        m_textures = std::move(other.m_textures);
        m_batch = std::move(other.m_batch);
        m_view_projection = other.m_view_projection;
        m_stats = other.m_stats;
        m_vao = std::exchange(other.m_vao, 0);
        m_vbo = std::exchange(other.m_vbo, 0);
        m_ebo = std::exchange(other.m_ebo, 0);
        m_quad_capacity = std::exchange(other.m_quad_capacity, 0);
        m_in_frame = std::exchange(other.m_in_frame, false);
    }
    return *this;
}

void Renderer2D::release() noexcept {
    if (m_ebo != 0) glDeleteBuffers(1, &m_ebo);
    if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
    m_vao = m_vbo = m_ebo = 0;
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

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capacity * 4 * sizeof(SpriteVertex)), nullptr,
                 GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo); // привязка EBO — часть состояния VAO
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
                 indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);

    m_quad_capacity = capacity;
}

TextureHandle Renderer2D::create_texture(const Image& image, const GL::TextureDesc& desc) {
    m_textures.push_back(GL::Texture::create(image, desc));
    return TextureHandle{static_cast<std::uint32_t>(m_textures.size() - 1)};
}

std::expected<TextureHandle, std::string> Renderer2D::load_texture(const std::filesystem::path& path,
                                                                   const GL::TextureDesc& desc) {
    auto image = Image::load(path);
    if (!image) {
        return std::unexpected(image.error());
    }
    return create_texture(*image, desc);
}

const GL::Texture& Renderer2D::texture(TextureHandle handle) const {
    if (handle.index >= m_textures.size()) {
        throw RendererError(std::format("Renderer2D: unknown texture handle {}", handle.index));
    }
    return m_textures[handle.index];
}

void Renderer2D::set_viewport(int width, int height) noexcept {
    glViewport(0, 0, width, height);
}

void Renderer2D::clear(Color color) noexcept {
    const glm::vec4 c = color.to_vec4();
    glClearColor(c.r, c.g, c.b, c.a);
    glClear(GL_COLOR_BUFFER_BIT);
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
    const auto vertices = m_batch.vertices();

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // Orphaning: драйвер выдаёт новый буфер, не дожидаясь GPU, который ещё читает прошлый кадр.
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_quad_capacity * 4 * sizeof(SpriteVertex)), nullptr,
                 GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vertices.size_bytes()), vertices.data());

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_shader.use();
    m_shader.set("u_view_projection", m_view_projection);
    m_shader.set("u_texture", 0);

    TextureHandle bound{std::numeric_limits<std::uint32_t>::max()}; // «ничего не привязано»
    for (const DrawCommand& command : commands) {
        if (command.texture != bound) {
            m_textures[command.texture.index].bind(0);
            bound = command.texture;
            ++m_stats.texture_binds;
        }
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(command.quad_count * 6), GL_UNSIGNED_INT,
                       byte_offset(std::size_t{command.first_quad} * 6 * sizeof(std::uint32_t)));
        ++m_stats.draw_calls;
    }

    glBindVertexArray(0);
    return m_stats;
}

} // namespace RendererSystem
