/**
 * @file OpenGLDevice.cpp
 * @brief Бэкенд RHI на OpenGL 3.3 core (glad).
 *
 * - Буфер = объект буфера GL; обновление — glBufferData (orphaning: драйвер не ждёт GPU).
 * - Раскладка вершины хранится в конвейере; VAO создаётся на пару (конвейер, буферы) и кэшируется.
 * - Блоки uniform: кольцо из буферов GL; блок «Frame» — точка привязки 0, «Draw» — 1, `u_texture` — блок текстур 0.
 * - Состояние конвейера (смешивание, глубина, отсечение) применяется только при смене конвейера.
 */

#include "../GL/Shader.hpp"
#include "Backends.hpp"

#include <RendererSystem/Core/Error.hpp>

#include <glad/glad.h>

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>

namespace RendererSystem::RHI {

namespace {

constexpr std::size_t ring_chunk_bytes = std::size_t{4} << 20;

GLint min_filter(const TextureDesc& desc) noexcept {
    if (desc.mipmaps) return desc.filter == TextureFilter::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
    return desc.filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
}

const void* byte_offset(std::size_t bytes) noexcept {
    return reinterpret_cast<const void*>(bytes); // NOLINT(performance-no-int-to-ptr): так OpenGL задаёт смещения
}

GLenum gl_usage(BufferUsage usage) noexcept {
    switch (usage) {
        case BufferUsage::Dynamic: return GL_DYNAMIC_DRAW;
        case BufferUsage::Stream: return GL_STREAM_DRAW;
        case BufferUsage::Static: break;
    }
    return GL_STATIC_DRAW;
}

class OpenGLDevice final : public Device {
public:
    OpenGLDevice() {
        m_info.backend = Backend::OpenGL;
        m_info.presents = true;
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        m_info.device_name = renderer != nullptr ? renderer : "?";
        m_info.api_version = version != nullptr ? version : "?";

        GLint align = 256;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
        m_uniform_align = static_cast<std::size_t>(std::max(align, 1));

        const Color white = Colors::white;
        m_white = make_texture(1, 1, TextureDesc{}, &white);
    }

    ~OpenGLDevice() override {
        for (auto& [key, vao] : m_vaos) glDeleteVertexArrays(1, &vao);
        m_buffers.for_each([](std::uint32_t, BufferRes& b) { glDeleteBuffers(1, &b.id); });
        m_targets.for_each([](std::uint32_t, TargetRes& t) {
            glDeleteFramebuffers(1, &t.fbo);
            if (t.depth != 0) glDeleteRenderbuffers(1, &t.depth);
        });
        m_textures.for_each([](std::uint32_t, TextureRes& t) { glDeleteTextures(1, &t.id); });
        m_pipelines.for_each([](std::uint32_t, PipelineRes& p) { p.shader.reset(); });
        for (const GLuint chunk : m_ring) glDeleteBuffers(1, &chunk);
        glDeleteTextures(1, &m_white);
    }

    // ------------------------------------------------------------------ буферы

    BufferId create_buffer(BufferKind kind, BufferUsage usage) override {
        BufferRes res{.kind = kind, .usage = usage};
        glGenBuffers(1, &res.id);
        return BufferId{m_buffers.add(res)};
    }

    void update_buffer(BufferId buffer, std::span<const std::byte> data) override {
        BufferRes& b = buffer_res(buffer);
        const GLenum target = b.kind == BufferKind::Index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
        glBindVertexArray(0); // привязка индексного буфера — часть состояния VAO: не портим чужой
        glBindBuffer(target, b.id);
        glBufferData(target, static_cast<GLsizeiptr>(data.size()), data.empty() ? nullptr : data.data(), gl_usage(b.usage));
        b.size = data.size();
        m_stats.uploaded_bytes += data.size();
    }

    void destroy_buffer(BufferId buffer) override {
        if (!m_buffers.contains(buffer.index)) return;
        forget_vaos([&](std::uint64_t key) { return ((key >> 20) & 0xFFFFF) == buffer.index || (key & 0xFFFFF) == buffer.index; });
        glDeleteBuffers(1, &m_buffers[buffer.index].id);
        m_buffers.remove(buffer.index);
    }

    // ------------------------------------------------------------------ текстуры

    TextureId create_texture(int width, int height, const TextureDesc& desc, std::span<const Color> pixels) override {
        if (width <= 0 || height <= 0) throw RendererError(std::format("Texture: invalid size {}x{}", width, height));
        if (!pixels.empty() && pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
            throw RendererError("Texture: pixel count does not match size");
        }
        const GLuint id = make_texture(width, height, desc, pixels.empty() ? nullptr : pixels.data());
        m_stats.uploaded_bytes += pixels.size_bytes();
        return TextureId{m_textures.add(TextureRes{id, width, height, desc, false})};
    }

    void update_texture(TextureId texture, std::span<const Color> pixels) override {
        TextureRes& t = texture_res(texture);
        if (pixels.size() != static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height)) {
            throw RendererError("Texture::update: pixel count does not match size");
        }
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.width, t.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        if (t.desc.mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_bound_texture = 0;
        m_stats.uploaded_bytes += pixels.size_bytes();
    }

    void generate_mipmaps(TextureId texture) override {
        TextureRes& t = texture_res(texture);
        if (!t.desc.mipmaps) return;
        glBindTexture(GL_TEXTURE_2D, t.id);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_bound_texture = 0;
    }

    void destroy_texture(TextureId texture) override {
        if (!m_textures.contains(texture.index) || m_textures[texture.index].owned_by_target) return;
        glDeleteTextures(1, &m_textures[texture.index].id);
        m_textures.remove(texture.index);
        m_bound_texture = 0;
    }

    // ------------------------------------------------------------------ цели

    std::expected<TargetId, std::string> create_target(int width, int height, const TargetDesc& desc) override {
        if (width <= 0 || height <= 0) return std::unexpected(std::format("invalid framebuffer size {}x{}", width, height));
        const TextureId color = create_texture(width, height, desc.color, {});
        m_textures[color.index].owned_by_target = true;

        GLint previous = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
        TargetRes res{.color = color, .width = width, .height = height};
        glGenFramebuffers(1, &res.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_textures[color.index].id, 0);
        if (desc.depth) {
            glGenRenderbuffers(1, &res.depth);
            glBindRenderbuffer(GL_RENDERBUFFER, res.depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, res.depth);
        }
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &res.fbo);
            if (res.depth != 0) glDeleteRenderbuffers(1, &res.depth);
            m_textures[color.index].owned_by_target = false;
            destroy_texture(color);
            return std::unexpected(std::format("framebuffer is incomplete (status {:#x})", status));
        }
        return TargetId{m_targets.add(res)};
    }

    TextureId target_texture(TargetId target) const override {
        if (!m_targets.contains(target.index)) throw RendererError("unknown render target");
        return m_targets[target.index].color;
    }

    void destroy_target(TargetId target) override {
        if (!m_targets.contains(target.index)) return;
        TargetRes& t = m_targets[target.index];
        if (m_current_target == target.index) bind_target({});
        glDeleteFramebuffers(1, &t.fbo);
        if (t.depth != 0) glDeleteRenderbuffers(1, &t.depth);
        m_textures[t.color.index].owned_by_target = false;
        destroy_texture(t.color);
        m_targets.remove(target.index);
    }

    UvRect target_uv() const noexcept override { return UvRect{{0.0f, 1.0f}, {1.0f, 0.0f}}; } // строка 0 текстуры GL — низ

    // ------------------------------------------------------------------ конвейеры

    std::expected<PipelineId, std::string> create_pipeline(const PipelineDesc& desc) override {
        const std::string vertex = std::string(glsl_prelude_opengl) + std::string(desc.shader.vertex);
        const std::string fragment = std::string(glsl_prelude_opengl) + std::string(desc.shader.fragment);
        auto shader = GL::Shader::from_source(vertex, fragment);
        if (!shader) return std::unexpected(std::format("pipeline '{}': {}", desc.name, shader.error()));
        const GLuint program = shader->id();
        if (const GLuint frame = glGetUniformBlockIndex(program, "Frame"); frame != GL_INVALID_INDEX) glUniformBlockBinding(program, frame, 0);
        if (const GLuint draw = glGetUniformBlockIndex(program, "Draw"); draw != GL_INVALID_INDEX) glUniformBlockBinding(program, draw, 1);
        glUseProgram(program);
        if (const GLint sampler = glGetUniformLocation(program, "u_texture"); sampler >= 0) glUniform1i(sampler, 0);
        glUseProgram(0);
        m_current_pipeline = 0;
        return PipelineId{m_pipelines.add(PipelineRes{std::make_shared<GL::Shader>(std::move(*shader)), desc})};
    }

    void destroy_pipeline(PipelineId pipeline) override {
        if (!m_pipelines.contains(pipeline.index)) return;
        forget_vaos([&](std::uint64_t key) { return (key >> 40) == pipeline.index; });
        if (m_current_pipeline == pipeline.index) m_current_pipeline = 0;
        m_pipelines.remove(pipeline.index);
    }

    // ------------------------------------------------------------------ кадр

    bool begin_frame(int width, int height) override {
        m_stats = {};
        m_screen_width = width;
        m_screen_height = height;
        // Кольцо uniform: новая память под тем же именем (orphaning) — прошлый кадр GPU дочитает старую.
        for (const GLuint chunk : m_ring) {
            glBindBuffer(GL_UNIFORM_BUFFER, chunk);
            glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(ring_chunk_bytes), nullptr, GL_STREAM_DRAW);
        }
        m_ring_chunk = 0;
        m_ring_offset = 0;
        m_current_pipeline = 0;
        m_bound_texture = 0;
        bind_target({});
        return width > 0 && height > 0;
    }

    void end_frame() override { glBindVertexArray(0); }

    void bind_target(TargetId target) override {
        if (target.valid()) {
            const TargetRes& t = target_res(target);
            glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
            glViewport(0, 0, t.width, t.height);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, m_screen_width, m_screen_height);
        }
        m_current_target = target.index;
        ++m_stats.passes;
    }

    void set_viewport(int width, int height) override {
        glViewport(0, 0, width, height);
        if (!m_current_target) m_screen_width = width, m_screen_height = height;
    }

    void clear(std::optional<Color> color, bool depth) override {
        GLbitfield mask = 0;
        if (color) {
            const glm::vec4 c = color->to_vec4();
            glClearColor(c.r, c.g, c.b, c.a);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (depth) {
            glDepthMask(GL_TRUE);
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        glClear(mask);
        m_current_pipeline = 0; // маска глубины могла поменяться — состояние конвейера применить заново
    }

    UniformSlice push_uniforms(std::span<const std::byte> data) override {
        const std::size_t size = data.size();
        if (size == 0) return {};
        std::size_t offset = (m_ring_offset + m_uniform_align - 1) / m_uniform_align * m_uniform_align;
        if (m_ring.empty() || offset + size > ring_chunk_bytes) {
            if (!m_ring.empty()) ++m_ring_chunk;
            if (m_ring_chunk >= m_ring.size()) {
                GLuint chunk = 0;
                glGenBuffers(1, &chunk);
                glBindBuffer(GL_UNIFORM_BUFFER, chunk);
                glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(ring_chunk_bytes), nullptr, GL_STREAM_DRAW);
                m_ring.push_back(chunk);
                m_ring_chunk = m_ring.size() - 1;
            }
            offset = 0;
        }
        glBindBuffer(GL_UNIFORM_BUFFER, m_ring[m_ring_chunk]);
        glBufferSubData(GL_UNIFORM_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data.data());
        m_ring_offset = offset + size;
        m_stats.uploaded_bytes += size;
        return UniformSlice{static_cast<std::uint32_t>(m_ring_chunk + 1), static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(size)};
    }

    void draw(const DrawCall& call) override {
        if (call.count == 0) return;
        PipelineRes& p = pipeline_res(call.pipeline);
        if (m_current_pipeline != call.pipeline.index) {
            apply_state(p);
            m_current_pipeline = call.pipeline.index;
            ++m_stats.pipeline_binds;
        }
        glBindVertexArray(vao_for(call, p));

        GLuint texture = m_white;
        if (call.texture.valid()) texture = texture_res(call.texture).id;
        if (texture != m_bound_texture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            m_bound_texture = texture;
        }
        if (call.frame.valid()) bind_slice(0, call.frame);
        if (!call.draw_uniforms.empty()) bind_slice(1, push_uniforms(call.draw_uniforms));

        const GLenum mode = p.desc.primitive == Primitive::Lines ? GL_LINES : GL_TRIANGLES;
        if (call.indices.valid()) {
            glDrawElements(mode, static_cast<GLsizei>(call.count), GL_UNSIGNED_INT, byte_offset(std::size_t{call.first} * sizeof(std::uint32_t)));
        } else {
            glDrawArrays(mode, static_cast<GLint>(call.first), static_cast<GLsizei>(call.count));
        }
        ++m_stats.draw_calls;
    }

    // ------------------------------------------------------------------ чтение

    Image read_target(TargetId target) override {
        const TargetRes& t = target_res(target);
        GLint previous = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        Image image(t.width, t.height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, t.width, t.height, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels().data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
        image.flip_vertically(); // в OpenGL строка 0 — низ
        return image;
    }

    Image read_screen() override {
        GLint previous = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        Image image(m_screen_width, m_screen_height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, m_screen_width, m_screen_height, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels().data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
        image.flip_vertically();
        return image;
    }

    void wait_idle() override { glFinish(); }

private:
    struct BufferRes {
        GLuint id = 0;
        BufferKind kind = BufferKind::Vertex;
        BufferUsage usage = BufferUsage::Static;
        std::size_t size = 0;
    };
    struct TextureRes {
        GLuint id = 0;
        int width = 0;
        int height = 0;
        TextureDesc desc{};
        bool owned_by_target = false;
    };
    struct TargetRes {
        GLuint fbo = 0;
        GLuint depth = 0;
        TextureId color{};
        int width = 0;
        int height = 0;
    };
    struct PipelineRes {
        std::shared_ptr<GL::Shader> shader;
        PipelineDesc desc{};
    };

    static GLuint make_texture(int width, int height, const TextureDesc& desc, const Color* pixels) {
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        const GLint wrap = desc.wrap == TextureWrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter(desc));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, desc.filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        if (desc.mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        return id;
    }

    BufferRes& buffer_res(BufferId id) {
        if (!m_buffers.contains(id.index)) throw RendererError(std::format("OpenGL device: unknown buffer {}", id.index));
        return m_buffers[id.index];
    }
    TextureRes& texture_res(TextureId id) {
        if (!m_textures.contains(id.index)) throw RendererError(std::format("OpenGL device: unknown texture {}", id.index));
        return m_textures[id.index];
    }
    const TargetRes& target_res(TargetId id) const {
        if (!m_targets.contains(id.index)) throw RendererError(std::format("OpenGL device: unknown render target {}", id.index));
        return m_targets[id.index];
    }
    PipelineRes& pipeline_res(PipelineId id) {
        if (!m_pipelines.contains(id.index)) throw RendererError(std::format("OpenGL device: unknown pipeline {}", id.index));
        return m_pipelines[id.index];
    }

    void apply_state(const PipelineRes& p) {
        p.shader->use();
        switch (p.desc.depth) {
            case DepthMode::Off: glDisable(GL_DEPTH_TEST); break;
            case DepthMode::Test:
                glEnable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
                break;
            case DepthMode::TestWrite:
                glEnable(GL_DEPTH_TEST);
                glDepthMask(GL_TRUE);
                break;
        }
        glDepthFunc(GL_LEQUAL);
        switch (p.desc.blend) {
            case BlendMode::Opaque: glDisable(GL_BLEND); break;
            case BlendMode::Alpha:
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case BlendMode::Additive:
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                break;
        }
        if (p.desc.cull == CullMode::Back) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
        } else {
            glDisable(GL_CULL_FACE);
        }
    }

    GLuint vao_for(const DrawCall& call, const PipelineRes& p) {
        const std::uint64_t key = (std::uint64_t{call.pipeline.index} << 40) | (std::uint64_t{call.vertices.index} << 20) | call.indices.index;
        if (const auto it = m_vaos.find(key); it != m_vaos.end()) return it->second;
        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, buffer_res(call.vertices).id);
        const auto stride = static_cast<GLsizei>(p.desc.layout.stride);
        for (const VertexAttribute& a : p.desc.layout.used()) {
            glEnableVertexAttribArray(a.location);
            const bool bytes = a.type == AttributeType::UnsignedByteNorm;
            glVertexAttribPointer(a.location, static_cast<GLint>(a.components), bytes ? GL_UNSIGNED_BYTE : GL_FLOAT,
                                  bytes ? GL_TRUE : GL_FALSE, stride, byte_offset(a.offset));
        }
        if (call.indices.valid()) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer_res(call.indices).id);
        m_vaos.emplace(key, vao);
        return vao;
    }

    template<typename Pred>
    void forget_vaos(Pred&& matches) {
        glBindVertexArray(0);
        for (auto it = m_vaos.begin(); it != m_vaos.end();) {
            if (matches(it->first)) {
                glDeleteVertexArrays(1, &it->second);
                it = m_vaos.erase(it);
            } else {
                ++it;
            }
        }
    }

    void bind_slice(GLuint binding, const UniformSlice& slice) {
        glBindBufferRange(GL_UNIFORM_BUFFER, binding, m_ring[slice.chunk - 1], static_cast<GLintptr>(slice.offset),
                          static_cast<GLsizeiptr>(slice.size));
    }

    SlotTable<BufferRes> m_buffers;
    SlotTable<TextureRes> m_textures;
    SlotTable<TargetRes> m_targets;
    SlotTable<PipelineRes> m_pipelines;
    std::unordered_map<std::uint64_t, GLuint> m_vaos;
    std::vector<GLuint> m_ring;
    std::size_t m_ring_chunk = 0;
    std::size_t m_ring_offset = 0;
    std::size_t m_uniform_align = 256;
    GLuint m_white = 0;
    GLuint m_bound_texture = 0;
    std::uint32_t m_current_pipeline = 0;
    std::uint32_t m_current_target = 0;
    int m_screen_width = 0;
    int m_screen_height = 0;
};

} // namespace

std::expected<std::unique_ptr<Device>, std::string> make_opengl_device(const DeviceConfig&) {
    if (glGetString == nullptr || glCreateShader == nullptr) {
        return std::unexpected(std::string("OpenGL functions are not loaded (is there a current context?)"));
    }
    return std::unique_ptr<Device>(new OpenGLDevice());
}

} // namespace RendererSystem::RHI
