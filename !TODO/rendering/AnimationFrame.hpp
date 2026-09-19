#pragma once

#include <glm/glm.hpp>

namespace rendering {

struct AnimationFrame {
    glm::vec2 uvOffset;      // Смещение UV координат (для спрайт-листов)
    glm::vec2 uvSize;        // Размер UV координат
    float duration;          // Длительность кадра в секундах
    
    AnimationFrame(const glm::vec2& offset = glm::vec2(0.0f), 
                   const glm::vec2& size = glm::vec2(1.0f),
                   float duration = 0.1f)
        : uvOffset(offset)
        , uvSize(size)
        , duration(duration)
    {}
};

} // namespace rendering