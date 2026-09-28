#include <RendererSystem/Core/Error.hpp>
#include <RendererSystem/GL/Texture.hpp>

#include <glad/glad.h>

#include <format>
#include <utility>

namespace RendererSystem::GL {

namespace {

GLint min_filter(const TextureDesc& desc) noexcept {
    if (desc.mipmaps) {
        return desc.filter == TextureFilter::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
    }
    return desc.filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
}

GLint mag_filter(const TextureDesc& desc) noexcept {
    return desc.filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
}

GLint wrap_mode(const TextureDesc& desc) noexcept {
    return desc.wrap == TextureWrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
}

GLuint create_storage(int width, int height, const void* pixels, const TextureDesc& desc) {
    if (width <= 0 || height <= 0) {
        throw RendererError(std::format("Texture: invalid size {}x{}", width, height));
    }
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter(desc));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter(desc));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(desc));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(desc));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); // строки RGBA8 всегда кратны 4 байтам
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (desc.mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return id;
}

} // namespace

Texture Texture::create(const Image& image, const TextureDesc& desc) {
    if (image.empty()) {
        throw RendererError("Texture: image is empty");
    }
    const GLuint id = create_storage(image.width(), image.height(), image.pixels().data(), desc);
    return Texture(id, image.width(), image.height(), desc);
}

Texture Texture::create_empty(int width, int height, const TextureDesc& desc) {
    const GLuint id = create_storage(width, height, nullptr, desc);
    return Texture(id, width, height, desc);
}

Texture::~Texture() {
    if (m_id != 0) {
        glDeleteTextures(1, &m_id);
    }
}

Texture::Texture(Texture&& other) noexcept
    : m_id(std::exchange(other.m_id, 0)),
      m_width(std::exchange(other.m_width, 0)),
      m_height(std::exchange(other.m_height, 0)),
      m_desc(other.m_desc) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (m_id != 0) {
            glDeleteTextures(1, &m_id);
        }
        m_id = std::exchange(other.m_id, 0);
        m_width = std::exchange(other.m_width, 0);
        m_height = std::exchange(other.m_height, 0);
        m_desc = other.m_desc;
    }
    return *this;
}

void Texture::update(const Image& image) {
    if (image.width() != m_width || image.height() != m_height) {
        throw RendererError(std::format("Texture::update: size {}x{} does not match texture {}x{}", image.width(),
                                        image.height(), m_width, m_height));
    }
    glBindTexture(GL_TEXTURE_2D, m_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels().data());
    if (m_desc.mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::bind(std::uint32_t unit) const noexcept {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_id);
}

} // namespace RendererSystem::GL
