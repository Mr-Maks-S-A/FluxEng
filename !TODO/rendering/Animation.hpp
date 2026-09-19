#pragma once

#include "AnimationFrame.hpp"
#include <string>
#include <vector>
#include <memory>

namespace rendering {

class Animation {
public:
    Animation(const std::string& name = "");
    ~Animation() = default;

    // Управление кадрами
    void addFrame(const AnimationFrame& frame);
    void addFramesFromGrid(int columns, int rows, int startFrame, int frameCount, 
                           float frameDuration, bool rowMajor = true);
    
    // Настройка анимации
    void setName(const std::string& name) { m_name = name; }
    void setLooping(bool looping) { m_looping = looping; }
    void setSpeed(float speed) { m_speed = speed; }
    
    // Получение информации
    const std::string& getName() const { return m_name; }
    const AnimationFrame& getFrame(int index) const;
    int getFrameCount() const { return static_cast<int>(m_frames.size()); }
    float getDuration() const;
    bool isLooping() const { return m_looping; }
    float getSpeed() const { return m_speed; }

private:
    std::string m_name;
    std::vector<AnimationFrame> m_frames;
    bool m_looping = true;
    float m_speed = 1.0f;
};

} // namespace rendering