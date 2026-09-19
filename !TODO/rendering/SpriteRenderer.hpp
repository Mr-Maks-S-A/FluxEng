#pragma once

#include "Shader.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <memory>

namespace rendering {

struct Vertex {
    glm::vec3 position;
    glm::vec2 texCoord;
};

class SpriteRenderer {
public:
    SpriteRenderer();
    ~SpriteRenderer();

    // Инициализация
    bool init();

    // Отрисовка
    void begin(const glm::mat4& view, const glm::mat4& projection);

    void drawSprite(const glm::vec2& position, const glm::vec2& size, float rotation,
                unsigned int textureID, const glm::vec4& color = glm::vec4(1.0f),
                const glm::vec2& uvOffset = glm::vec2(0.0f),
                const glm::vec2& uvSize = glm::vec2(1.0f));
    void end();

    // Геттеры
    Shader& getShader() { return m_shader; }

private:
    Shader m_shader;
    unsigned int m_VAO;
    unsigned int m_VBO;
    unsigned int m_EBO;
    
    glm::mat4 m_view;
    glm::mat4 m_projection;
    
    void setupMesh();
};

} // namespace rendering
