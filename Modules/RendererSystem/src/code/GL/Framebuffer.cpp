#include <RendererSystem/GL/Framebuffer.hpp>

#include <glad/glad.h>

#include <format>

namespace RendererSystem::GL {

std::expected<Framebuffer, std::string> Framebuffer::create(int width, int height, const TextureDesc& desc) {
    if (glGenFramebuffers == nullptr) {
        return std::unexpected(std::string("OpenGL functions are not loaded (is there a current context?)"));
    }
    if (width <= 0 || height <= 0) {
        return std::unexpected(std::format("invalid framebuffer size {}x{}", width, height));
    }

    Texture color = Texture::create_empty(width, height, desc);

    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);

    GLuint id = 0;
    glGenFramebuffers(1, &id);
    glBindFramebuffer(GL_FRAMEBUFFER, id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.id(), 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &id);
        return std::unexpected(std::format("framebuffer is incomplete (status {:#x})", status));
    }
    return Framebuffer(id, std::move(color));
}

Framebuffer::~Framebuffer() {
    if (m_id != 0) {
        glDeleteFramebuffers(1, &m_id);
    }
}

Framebuffer::Framebuffer(Framebuffer&& other) noexcept
    : m_id(std::exchange(other.m_id, 0)), m_color(std::move(other.m_color)) {}

Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept {
    if (this != &other) {
        if (m_id != 0) {
            glDeleteFramebuffers(1, &m_id);
        }
        m_id = std::exchange(other.m_id, 0);
        m_color = std::move(other.m_color);
    }
    return *this;
}

void Framebuffer::bind() const noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, m_id);
    glViewport(0, 0, width(), height());
}

void Framebuffer::bind_default() noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

Image Framebuffer::read_pixels() const {
    GLint previous = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);

    Image image(width(), height());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_id);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width(), height(), GL_RGBA, GL_UNSIGNED_BYTE, image.pixels().data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));

    image.flip_vertically(); // в OpenGL строка 0 — низ
    return image;
}

} // namespace RendererSystem::GL
