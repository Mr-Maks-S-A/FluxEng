// Modules/RendererSystem/src/include/RendererSystem/IRenderer.hpp
#pragma once

#include <WindowSystem/Window.hpp>
#include <string>
#include <cstdint>

struct Sprite;
struct Position;

namespace Renderer {

    class IRenderer {
    public:
        virtual ~IRenderer() = default;

        // Инициализируем рендер, передавая ему существующее окно
        virtual bool initialize(Window& window) = 0;

        virtual void beginFrame() = 0;
        virtual void drawSprite(const Sprite& sprite, const Position& position, 
                                float rotation = 0.0f, float scale = 1.0f) = 0;
        virtual void drawText(const std::string& text, float x, float y, 
                              int size = 16, uint32_t color = 0xFFFFFFFF) = 0;
        virtual void drawLine(float x1, float y1, float x2, float y2, 
                              uint32_t color = 0xFFFFFFFF) = 0;
        virtual void drawRect(float x, float y, float width, float height,
                              uint32_t color = 0xFFFFFFFF) = 0;
        virtual void fillRect(float x, float y, float width, float height,
                              uint32_t color = 0xFFFFFFFF) = 0;
        virtual void endFrame() = 0;
        virtual void shutdown() = 0;

        virtual float getDeltaTime() = 0;
    };

} // namespace Renderer