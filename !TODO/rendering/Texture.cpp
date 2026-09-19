#include "Texture.hpp"

// Заменяем fpng на stb_image
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>

namespace rendering {

Texture::Texture() {
    glGenTextures(1, &m_id);
}

Texture::~Texture() {
    destroy();
}

void Texture::destroy() {
    if (m_id) {
        glDeleteTextures(1, &m_id);
        m_id = 0;
    }
}

Texture::Texture(Texture&& other) noexcept {
    m_id = other.m_id;
    m_width = other.m_width;
    m_height = other.m_height;
    other.m_id = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        destroy();
        m_id = other.m_id;
        m_width = other.m_width;
        m_height = other.m_height;
        other.m_id = 0;
    }
    return *this;
}

bool Texture::loadFromFile(const std::string& path, bool flipY)
{
    // Устанавливаем флаг переворота изображения
    stbi_set_flip_vertically_on_load(flipY ? 1 : 0);
    
    int width, height, channels;
    
    // Загружаем изображение (stbi автоматически определяет формат)
    unsigned char* data = stbi_load(
        path.c_str(),
        &width,
        &height,
        &channels,
        STBI_rgb_alpha  // Принудительно загружаем как RGBA (4 канала)
    );
    
    if (!data) {
        std::cerr << "Failed to load image: " << path << std::endl;
        std::cerr << "STB Error: " << stbi_failure_reason() << std::endl;
        return false;
    }
    
    m_width = width;
    m_height = height;
    
    glBindTexture(GL_TEXTURE_2D, m_id);
    
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        m_width,
        m_height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        data
    );
    
    glGenerateMipmap(GL_TEXTURE_2D);
    setFiltering(GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Освобождаем память
    stbi_image_free(data);
    
    return true;
}

void Texture::createFromMemory(unsigned char* data, int width, int height) {
    m_width = width;
    m_height = height;
    
    glBindTexture(GL_TEXTURE_2D, m_id);
    
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA8,
                 width,
                 height,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 data);
    
    setFiltering(GL_NEAREST, GL_NEAREST);
    
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Перегрузка для загрузки из памяти с данными STB
void Texture::createFromMemory(unsigned char* data, int width, int height, int channels) {
    m_width = width;
    m_height = height;
    
    GLenum format = GL_RGBA;
    if (channels == 3) format = GL_RGB;
    else if (channels == 1) format = GL_RED;
    
    glBindTexture(GL_TEXTURE_2D, m_id);
    
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 format == GL_RGBA ? GL_RGBA8 : (format == GL_RGB ? GL_RGB8 : GL_R8),
                 width,
                 height,
                 0,
                 format,
                 GL_UNSIGNED_BYTE,
                 data);
    
    setFiltering(GL_LINEAR, GL_LINEAR);
    
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::bind(unsigned int slot) const {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, m_id);
}

void Texture::unbind() {
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::setFiltering(GLint minFilter, GLint magFilter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

} // namespace rendering
