// Modules/RendererSystem/src/code/OpenGLRenderer.cpp
#include <RendererSystem/OpenGLRenderer.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

namespace Renderer {

// Встроенные стандартные 2D-шейдеры, чтобы не зависеть от внешних файлов
static const char* defaultVertexShaderSource = R"(
    #version 330 core
    layout (location = 0) in vec2 aPos;
    layout (location = 1) in vec2 aTexCoord;
    layout (location = 2) in vec4 aColor;

    out vec2 TexCoord;
    out vec4 Color;

    uniform mat4 uProjection;
    uniform mat4 uView;

    void main() {
        gl_Position = uProjection * uView * vec4(aPos, 0.0, 1.0);
        TexCoord = aTexCoord;
        Color = aColor;
    }
)";

static const char* defaultFragmentShaderSource = R"(
    #version 330 core
    in vec2 TexCoord;
    in vec4 Color;

    out vec4 FragColor;

    uniform sampler2D uTexture;
    uniform bool uUseTexture;

    void main() {
        if (uUseTexture) {
            FragColor = texture(uTexture, TexCoord) * Color;
        } else {
            FragColor = Color;
        }
    }
)";

OpenGLRenderer::~OpenGLRenderer() {
    shutdown();
}

bool OpenGLRenderer::initialize(Window& window) {
    m_window = &window;

    // Включаем смешивание для прозрачности (Alpha blending)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (!initShaders()) return false;
    if (!initBuffers()) return false;

    // Создаем ортографическую проекцию 2D для размеров окна
    m_projectionMatrix = glm::ortho(0.0f, static_cast<float>(window.getWidth()),
                                    static_cast<float>(window.getHeight()), 0.0f,
                                    -1.0f, 1.0f);
    m_lastFrameTime = static_cast<float>(glfwGetTime());

    return true;
}

bool OpenGLRenderer::initShaders() {
    m_spriteShader = std::make_unique<Shader>();
    if (!m_spriteShader->loadFromSource(defaultVertexShaderSource, defaultFragmentShaderSource)) {
        std::cerr << "[OpenGLRenderer Error] Не удалось скомпилировать встроенные шейдеры!\n";
        return false;
    }
    return true;
}

bool OpenGLRenderer::initBuffers() {
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // Выделяем память под максимальное количество вершин батча
    glBufferData(GL_ARRAY_BUFFER, m_maxSpritesPerBatch * 4 * sizeof(Vertex2D), nullptr, GL_DYNAMIC_DRAW);

    // Подготовка индексов (quad index buffer)
    std::vector<GLuint> indices(m_maxSpritesPerBatch * 6);
    GLuint offset = 0;
    for (size_t i = 0; i < indices.size(); i += 6) {
        indices[i + 0] = offset + 0;
        indices[i + 1] = offset + 1;
        indices[i + 2] = offset + 2;
        indices[i + 3] = offset + 0;
        indices[i + 4] = offset + 2;
        indices[i + 5] = offset + 3;
        offset += 4;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint), indices.data(), GL_STATIC_DRAW);

    // Атрибуты вершин
    Vertex2D::setupAttributes();

    glBindVertexArray(0);
    return true;
}

void OpenGLRenderer::beginFrame() {
    float currentFrameTime = static_cast<float>(glfwGetTime());
    m_deltaTime = currentFrameTime - m_lastFrameTime;
    m_lastFrameTime = currentFrameTime;

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_spriteVertices.clear();
}

void OpenGLRenderer::flushSprites() {
    if (m_spriteVertices.empty()) return;

    m_spriteShader->use();
    m_spriteShader->setMat4("uProjection", glm::value_ptr(m_projectionMatrix));
    m_spriteShader->setMat4("uView", glm::value_ptr(m_viewMatrix));
    m_spriteShader->setBool("uUseTexture", false);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, m_spriteVertices.size() * sizeof(Vertex2D), m_spriteVertices.data());

    size_t indexCount = (m_spriteVertices.size() / 4) * 6;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);
    m_spriteVertices.clear();
}

void OpenGLRenderer::drawSprite(const Sprite& sprite, const Position& position, float rotation, float scale) {
    // В будущем здесь будет добавление данных прямоугольника с текстурой в m_spriteVertices
}

void OpenGLRenderer::drawText(const std::string& text, float x, float y, int size, uint32_t color) {
    // Место для реализации шрифтов (например через FreeType)
}

void OpenGLRenderer::drawLine(float x1, float y1, float x2, float y2, uint32_t color) {
    // Примитивная линия
    flushSprites(); // Очищаем накапливаемый батч

    Vertex2D lineVertices[2] = {
        Vertex2D(x1, y1, 0, 0, (color >> 24)&0xFF, (color >> 16)&0xFF, (color >> 8)&0xFF, color & 0xFF),
        Vertex2D(x2, y2, 0, 0, (color >> 24)&0xFF, (color >> 16)&0xFF, (color >> 8)&0xFF, color & 0xFF)
    };

    m_spriteShader->use();
    m_spriteShader->setMat4("uProjection", glm::value_ptr(m_projectionMatrix));
    m_spriteShader->setMat4("uView", glm::value_ptr(m_viewMatrix));
    m_spriteShader->setBool("uUseTexture", false);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(lineVertices), lineVertices);

    glDrawArrays(GL_LINES, 0, 2);
    glBindVertexArray(0);
}

void OpenGLRenderer::drawRect(float x, float y, float width, float height, uint32_t color) {
    drawLine(x, y, x + width, y, color);
    drawLine(x + width, y, x + width, y + height, color);
    drawLine(x + width, y + height, x, y + height, color);
    drawLine(x, y + height, x, y, color);
}

void OpenGLRenderer::fillRect(float x, float y, float width, float height, uint32_t color) {
    if (m_spriteVertices.size() / 4 >= m_maxSpritesPerBatch) {
        flushSprites();
    }

    Quad quad(x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f, color);
    m_spriteVertices.insert(m_spriteVertices.end(), quad.vertices.begin(), quad.vertices.end());
}

void OpenGLRenderer::endFrame() {
    flushSprites();
}

void OpenGLRenderer::shutdown() {
    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
    if (m_ebo != 0) glDeleteBuffers(1, &m_ebo);
    m_vao = m_vbo = m_ebo = 0;
}

} // namespace Renderer