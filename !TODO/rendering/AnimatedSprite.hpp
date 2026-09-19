#pragma once

#include "Animation.hpp"
#include "Texture.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

namespace rendering {

class AnimatedSprite {
public:
    AnimatedSprite();
    explicit AnimatedSprite(std::shared_ptr<Texture> texture);
    ~AnimatedSprite() = default;

    // Управление анимациями
    void addAnimation(std::unique_ptr<Animation> animation);
    void setCurrentAnimation(const std::string& name);
    void setCurrentAnimation(int index);
    
    // Обновление и отрисовка
    void update(float deltaTime);
    void draw(class SpriteRenderer& renderer, const glm::vec2& position, 
              const glm::vec2& size, float rotation = 0.0f,
              const glm::vec4& color = glm::vec4(1.0f));
    
    // Управление воспроизведением
    void play(const std::string& name = "");
    void pause();
    void resume();
    void stop();
    void reset();
    
    // Настройки
    void setTexture(std::shared_ptr<Texture> texture) { m_texture = texture; }
    void setFlipX(bool flip) { m_flipX = flip; }
    void setFlipY(bool flip) { m_flipY = flip; }
    void setColor(const glm::vec4& color) { m_color = color; }
    
    // Состояние
    bool isPlaying() const { return m_isPlaying; }
    bool isPaused() const { return m_isPaused; }
    const std::string& getCurrentAnimationName() const;
    int getCurrentFrame() const { return m_currentFrame; }
    float getProgress() const;

private:
    std::shared_ptr<Texture> m_texture;
    std::unordered_map<std::string, std::unique_ptr<Animation>> m_animations;
    std::vector<std::string> m_animationOrder; // Для доступа по индексу
    
    Animation* m_currentAnimation = nullptr;
    int m_currentFrame = 0;
    float m_frameTimer = 0.0f;
    
    bool m_isPlaying = false;
    bool m_isPaused = false;
    bool m_flipX = false;
    bool m_flipY = false;
    glm::vec4 m_color = glm::vec4(1.0f);
    
    void updateUVs(glm::vec2& uvOffset, glm::vec2& uvSize) const;
};

} // namespace rendering