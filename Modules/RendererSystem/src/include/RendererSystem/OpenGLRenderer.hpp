// Modules/RendererSystem/src/include/RendererSystem/OpenGLRenderer.hpp
#pragma once

#include <RendererSystem/IRenderer.hpp>
#include <RendererSystem/Shader.hpp>
#include <RendererSystem/Texture.hpp>
#include <RendererSystem/Vertex.hpp>

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <unordered_map>
#include <memory>
#include <vector>

namespace Renderer {

    class OpenGLRenderer : public IRenderer {
    private:
        Window* m_window = nullptr;
        
        float m_deltaTime = 0.0f;
        float m_lastFrameTime = 0.0f;

        std::unique_ptr<Shader> m_spriteShader;
        std::unique_ptr<Shader> m_lineShader;

        std::unordered_map<std::string, std::unique_ptr<Texture>> m_textureCache;

        std::vector<Vertex2D> m_spriteVertices;
        std::vector<GLuint> m_spriteIndices;
        unsigned int m_vao = 0;
        unsigned int m_vbo = 0;
        unsigned int m_ebo = 0;
        bool m_batchingEnabled = true;
        size_t m_maxSpritesPerBatch = 1000;

        glm::mat4 m_projectionMatrix{1.0f};
        glm::mat4 m_viewMatrix{1.0f};

        bool initOpenGL();
        bool initShaders();
        bool initBuffers();
        void flushSprites();
        Texture* getOrLoadTexture(const std::string& textureName);

    public:
        OpenGLRenderer() = default;
        ~OpenGLRenderer() override;

        bool initialize(Window& window) override;
        void beginFrame() override;
        void drawSprite(const Sprite& sprite, const Position& position,
                                float rotation = 0.0f, float scale = 1.0f) override;
        void drawText(const std::string& text, float x, float y,
                              int size = 16, uint32_t color = 0xFFFFFFFF) override;
        void drawLine(float x1, float y1, float x2, float y2,
                              uint32_t color = 0xFFFFFFFF) override;
        void drawRect(float x, float y, float width, float height,
                              uint32_t color = 0xFFFFFFFF) override;
        void fillRect(float x, float y, float width, float height,
                              uint32_t color = 0xFFFFFFFF) override;
        void endFrame() override;
        void shutdown() override;
        float getDeltaTime() override { return m_deltaTime; }

        void setBatchingEnabled(bool enabled) { m_batchingEnabled = enabled; }
        void setMaxSpritesPerBatch(size_t max) { m_maxSpritesPerBatch = max; }
        void setView(const glm::mat4& view) { m_viewMatrix = view; }
        void setProjection(const glm::mat4& proj) { m_projectionMatrix = proj; }
    };

} // namespace Renderer