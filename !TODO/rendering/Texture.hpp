#pragma once

#include <string>
#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace rendering {

class Texture {
public:
    Texture();
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    bool loadFromFile(const std::string& path, bool flipY = true);
    void createFromMemory(unsigned char* data, int width, int height);
    
    // Новая перегрузка для данных с произвольным количеством каналов
    void createFromMemory(unsigned char* data, int width, int height, int channels);

    void bind(unsigned int slot = 0) const;
    static void unbind();

    void setFiltering(GLint minFilter, GLint magFilter);

    unsigned int id() const { return m_id; }
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    unsigned int m_id = 0;
    int m_width = 0;
    int m_height = 0;

    void destroy();
};

} // namespace rendering
