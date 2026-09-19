#include "SpriteRenderer.hpp"
#include <iostream>
// GLAD (должен быть перед GLFW)
#include <glad/gl.h>

// GLFW
#include <GLFW/glfw3.h>

// GLM
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


namespace rendering {

SpriteRenderer::SpriteRenderer() 
    : m_VAO(0)
    , m_VBO(0)
    , m_EBO(0)
{
}

SpriteRenderer::~SpriteRenderer() {
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_EBO) glDeleteBuffers(1, &m_EBO);
}

bool SpriteRenderer::init() {
    // Загружаем базовый шейдер для спрайтов
    // Пока используем встроенные шейдеры для теста
    const char* vertexShaderSource = R"(
    #version 330 core
    layout (location = 0) in vec3 aPos;
    layout (location = 1) in vec2 aTexCoord;
    
    out vec2 TexCoord;
    
    uniform mat4 model;
    uniform mat4 view;
    uniform mat4 projection;
    uniform vec2 uvOffset;
    uniform vec2 uvSize;
    
    void main() {
        gl_Position = projection * view * model * vec4(aPos, 1.0);
        TexCoord = uvOffset + aTexCoord * uvSize;
    }
)";

    const char* fragmentShaderSource = R"(
    #version 330 core
    in vec2 TexCoord;
    out vec4 FragColor;
    
    uniform sampler2D texture1;
    uniform vec4 color;
    
    void main() {
        FragColor = texture(texture1, TexCoord) * color;
    }
)";

    if (!m_shader.loadFromSource(vertexShaderSource, fragmentShaderSource)) {
        std::cerr << "Failed to load sprite shader" << std::endl;
        return false;
    }

    setupMesh();
    return true;
}

void SpriteRenderer::setupMesh() {
    // Вершины для прямоугольника (спрайта)
    Vertex vertices[4] = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}}, // нижний левый
        {{ 0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}}, // нижний правый
        {{ 0.5f,  0.5f, 0.0f}, {1.0f, 1.0f}}, // верхний правый
        {{-0.5f,  0.5f, 0.0f}, {0.0f, 1.0f}}  // верхний левый
    };

    // Индексы для двух треугольников
    unsigned int indices[6] = {
        0, 1, 2,  // первый треугольник
        2, 3, 0   // второй треугольник
    };

    // Создаем VAO
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glGenBuffers(1, &m_EBO);

    glBindVertexArray(m_VAO);

    // Загружаем вершинные данные
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // Загружаем индексные данные
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // Настраиваем атрибуты вершин
    // Позиция
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Текстурные координаты
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texCoord));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void SpriteRenderer::begin(const glm::mat4& view, const glm::mat4& projection) {
    m_view = view;
    m_projection = projection;
    
    m_shader.use();
    m_shader.setUniform("view", m_view);
    m_shader.setUniform("projection", m_projection);
    
    glBindVertexArray(m_VAO);
    glActiveTexture(GL_TEXTURE0);
}

// Добавьте новый метод:
void SpriteRenderer::drawSprite(const glm::vec2& position, const glm::vec2& size, float rotation,
                                 unsigned int textureID, const glm::vec4& color,
                                 const glm::vec2& uvOffset, const glm::vec2& uvSize) {
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, glm::vec3(position.x, position.y, 0.0f));
    model = glm::rotate(model, glm::radians(rotation), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, glm::vec3(size.x, size.y, 1.0f));
    
    m_shader.setUniform("model", model);
    m_shader.setUniform("color", color);
    m_shader.setUniform("uvOffset", uvOffset);
    m_shader.setUniform("uvSize", uvSize);
    
    glBindTexture(GL_TEXTURE_2D, textureID);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

void SpriteRenderer::end() {
    glBindVertexArray(0);
    m_shader.unuse();
}

} // namespace rendering
