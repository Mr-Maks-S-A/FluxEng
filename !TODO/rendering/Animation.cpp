#include "Animation.hpp"
#include <algorithm>
#include <stdexcept>

namespace rendering {

Animation::Animation(const std::string& name)
    : m_name(name)
{
}

void Animation::addFrame(const AnimationFrame& frame) {
    m_frames.push_back(frame);
}

void Animation::addFramesFromGrid(int columns, int rows, int startFrame, int frameCount,
                                  float frameDuration, bool rowMajor) {
    float frameWidth = 1.0f / columns;
    float frameHeight = 1.0f / rows;
    
    for (int i = 0; i < frameCount; ++i) {
        int frameIndex = startFrame + i;
        int row, col;
        
        if (rowMajor) {
            row = frameIndex / columns;
            col = frameIndex % columns;
        } else {
            col = frameIndex / rows;
            row = frameIndex % rows;
        }
        
        // Проверка границ
        if (row >= rows || col >= columns) {
            break;
        }
        
        glm::vec2 uvOffset(col * frameWidth, row * frameHeight);
        glm::vec2 uvSize(frameWidth, frameHeight);
        
        addFrame(AnimationFrame(uvOffset, uvSize, frameDuration));
    }
}

const AnimationFrame& Animation::getFrame(int index) const {
    if (index < 0 || index >= static_cast<int>(m_frames.size())) {
        throw std::out_of_range("Animation frame index out of range");
    }
    return m_frames[index];
}

float Animation::getDuration() const {
    float total = 0.0f;
    for (const auto& frame : m_frames) {
        total += frame.duration;
    }
    return total / m_speed;
}

} // namespace rendering