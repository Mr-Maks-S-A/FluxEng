// Renderer/Vertex.h
#pragma once

#include <cstdint>
#include <array>

namespace Renderer {

    /**
     * @brief 2D вершина для OpenGL
     * Содержит позицию, текстурные координаты и цвет
     */
    struct Vertex2D {
        float x, y;           // Позиция
        float u, v;           // Текстурные координаты
        uint8_t r, g, b, a;   // Цвет (RGBA)
        
        Vertex2D(float x = 0, float y = 0, 
                 float u = 0, float v = 0,
                 uint8_t r = 255, uint8_t g = 255, uint8_t b = 255, uint8_t a = 255)
            : x(x), y(y), u(u), v(v), r(r), g(g), b(b), a(a) {}
        
        // Для использования с glVertexAttribPointer
        static constexpr int getAttributeCount() { return 3; } // позиция, UV, цвет
        static constexpr int getStride() { return sizeof(Vertex2D); }
        
        static void setupAttributes() {
            // Позиция (2 floats)
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, getStride(), (void*)0);
            glEnableVertexAttribArray(0);
            
            // Текстурные координаты (2 floats)
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, getStride(), 
                                  (void*)(2 * sizeof(float)));
            glEnableVertexAttribArray(1);
            
            // Цвет (4 bytes)
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, getStride(),
                                  (void*)(4 * sizeof(float)));
            glEnableVertexAttribArray(2);
        }
    };

    /**
     * @brief Прямоугольник, составленный из двух треугольников
     * Используется для отрисовки спрайтов
     */
    struct Quad {
        std::array<Vertex2D, 4> vertices;
        std::array<GLuint, 6> indices = {0, 1, 2, 0, 2, 3};
        
        Quad(float x, float y, float width, float height,
             float u1 = 0, float v1 = 0, float u2 = 1, float v2 = 1,
             uint32_t color = 0xFFFFFFFF) {
            
            // Распаковка цвета
            uint8_t r = (color >> 24) & 0xFF;
            uint8_t g = (color >> 16) & 0xFF;
            uint8_t b = (color >> 8) & 0xFF;
            uint8_t a = color & 0xFF;
            
            // Вершины в порядке: bottom-left, bottom-right, top-right, top-left
            vertices[0] = Vertex2D(x,         y,          u1, v2, r, g, b, a);
            vertices[1] = Vertex2D(x + width, y,          u2, v2, r, g, b, a);
            vertices[2] = Vertex2D(x + width, y + height, u2, v1, r, g, b, a);
            vertices[3] = Vertex2D(x,         y + height, u1, v1, r, g, b, a);
        }
    };

} // namespace Renderer