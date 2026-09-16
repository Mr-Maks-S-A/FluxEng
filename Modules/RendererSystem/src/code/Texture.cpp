// Modules/RendererSystem/src/code/Texture.cpp
#include <RendererSystem/Texture.hpp>
#include <glad/glad.h>
#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace Renderer {

Texture::Texture() : m_textureID(0), m_width(0), m_height(0) {}

Texture::~Texture() {
    if (m_textureID != 0) {
        glDeleteTextures(1, &m_textureID);
    }
}

Texture::Texture(Texture&& other) noexcept
    : m_textureID(other.m_textureID), m_width(other.m_width),
      m_height(other.m_height), m_filePath(std::move(other.m_filePath)) 
{
    other.m_textureID = 0;
    other.m_width = 0;
    other.m_height = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (m_textureID != 0) {
            glDeleteTextures(1, &m_textureID);
        }
        m_textureID = other.m_textureID;
        m_width = other.m_width;
        m_height = other.m_height;
        m_filePath = std::move(other.m_filePath);

        other.m_textureID = 0;
        other.m_width = 0;
        other.m_height = 0;
    }
    return *this;
}

bool Texture::loadFromFile(const std::string& filePath) {
    m_filePath = filePath;

    stbi_set_flip_vertically_on_load(true);

    int channels;
    unsigned char* data = stbi_load(filePath.c_str(), &m_width, &m_height, &channels, 0);
    if (!data) {
        std::cerr << "[Texture Error] Не удалось загрузить изображение: " << filePath << "\n";
        return false;
    }

    GLenum format = GL_RGB;
    if (channels == 1) format = GL_RED;
    else if (channels == 3) format = GL_RGB;
    else if (channels == 4) format = GL_RGBA;

    glGenTextures(1, &m_textureID);
    glBindTexture(GL_TEXTURE_2D, m_textureID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, format, m_width, m_height, 0, format, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    stbi_image_free(data);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

void Texture::create(int width, int height, const uint8_t* data) {
    m_width = width;
    m_height = height;

    if (m_textureID != 0) {
        glDeleteTextures(1, &m_textureID);
    }

    glGenTextures(1, &m_textureID);
    glBindTexture(GL_TEXTURE_2D, m_textureID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::bind(int textureUnit) const {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, m_textureID);
}

void Texture::unbind() const {
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace Renderer