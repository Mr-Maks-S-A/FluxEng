#include "AnimatedSprite.hpp"
#include "SpriteRenderer.hpp"
#include <stdexcept>

namespace rendering {

AnimatedSprite::AnimatedSprite()
    : m_texture(nullptr)
{
}

AnimatedSprite::AnimatedSprite(std::shared_ptr<Texture> texture)
    : m_texture(texture)
{
}

void AnimatedSprite::addAnimation(std::unique_ptr<Animation> animation) {
    if (!animation) return;
    
    std::string name = animation->getName();
    if (name.empty()) {
        // Генерируем имя, если его нет
        name = "animation_" + std::to_string(m_animations.size());
        animation->setName(name);
    }
    
    m_animations[name] = std::move(animation);
    m_animationOrder.push_back(name);
    
    // Если это первая анимация, устанавливаем её текущей
    if (!m_currentAnimation) {
        setCurrentAnimation(name);
    }
}

void AnimatedSprite::setCurrentAnimation(const std::string& name) {
    auto it = m_animations.find(name);
    if (it != m_animations.end()) {
        m_currentAnimation = it->second.get();
        reset();
    }
}

void AnimatedSprite::setCurrentAnimation(int index) {
    if (index >= 0 && index < static_cast<int>(m_animationOrder.size())) {
        setCurrentAnimation(m_animationOrder[index]);
    }
}

void AnimatedSprite::update(float deltaTime) {
    if (!m_currentAnimation || !m_isPlaying || m_isPaused) {
        return;
    }
    
    const auto& frames = m_currentAnimation->getFrames();
    if (frames.empty()) return;
    
    float speed = m_currentAnimation->getSpeed();
    m_frameTimer += deltaTime * speed;
    
    float currentFrameDuration = frames[m_currentFrame].duration;
    
    while (m_frameTimer >= currentFrameDuration) {
        m_frameTimer -= currentFrameDuration;
        m_currentFrame++;
        
        if (m_currentFrame >= static_cast<int>(frames.size())) {
            if (m_currentAnimation->isLooping()) {
                m_currentFrame = 0;
            } else {
                m_currentFrame = static_cast<int>(frames.size()) - 1;
                m_isPlaying = false;
                break;
            }
        }
        
        currentFrameDuration = frames[m_currentFrame].duration;
    }
}

void AnimatedSprite::draw(SpriteRenderer& renderer, const glm::vec2& position,
                          const glm::vec2& size, float rotation, const glm::vec4& color) {
    if (!m_texture || !m_currentAnimation) return;
    
    glm::vec2 uvOffset, uvSize;
    updateUVs(uvOffset, uvSize);
    
    // Здесь нужно модифицировать SpriteRenderer для поддержки UV смещений
    // Пока будем использовать стандартный метод с дополнительными параметрами
    
    // Временно сохраняем текущую проекцию
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, glm::vec3(position.x, position.y, 0.0f));
    model = glm::rotate(model, glm::radians(rotation), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, glm::vec3(size.x, size.y, 1.0f));
    
    // Применяем флип
    if (m_flipX || m_flipY) {
        model = glm::scale(model, glm::vec3(m_flipX ? -1.0f : 1.0f, 
                                             m_flipY ? -1.0f : 1.0f, 
                                             1.0f));
    }
    
    // Устанавливаем uniform'ы для UV
    auto& shader = renderer.getShader();
    shader.setUniform("uvOffset", uvOffset);
    shader.setUniform("uvSize", uvSize);
    
    renderer.drawSprite(position, size, rotation, m_texture->id(), m_color * color);
}

void AnimatedSprite::play(const std::string& name) {
    if (!name.empty()) {
        setCurrentAnimation(name);
    }
    
    if (m_currentAnimation && !m_currentAnimation->getFrames().empty()) {
        m_isPlaying = true;
        m_isPaused = false;
    }
}

void AnimatedSprite::pause() {
    m_isPaused = true;
}

void AnimatedSprite::resume() {
    m_isPaused = false;
    m_isPlaying = true;
}

void AnimatedSprite::stop() {
    m_isPlaying = false;
    m_isPaused = false;
    m_currentFrame = 0;
    m_frameTimer = 0.0f;
}

void AnimatedSprite::reset() {
    m_currentFrame = 0;
    m_frameTimer = 0.0f;
    m_isPlaying = false;
    m_isPaused = false;
}

const std::string& AnimatedSprite::getCurrentAnimationName() const {
    static const std::string empty;
    if (m_currentAnimation) {
        return m_currentAnimation->getName();
    }
    return empty;
}

float AnimatedSprite::getProgress() const {
    if (!m_currentAnimation || m_currentAnimation->getFrames().empty()) {
        return 0.0f;
    }
    
    float totalDuration = 0.0f;
    for (int i = 0; i < m_currentFrame; ++i) {
        totalDuration += m_currentAnimation->getFrame(i).duration;
    }
    totalDuration += m_frameTimer;
    
    return totalDuration / m_currentAnimation->getDuration();
}

void AnimatedSprite::updateUVs(glm::vec2& uvOffset, glm::vec2& uvSize) const {
    if (!m_currentAnimation || m_currentAnimation->getFrames().empty()) {
        uvOffset = glm::vec2(0.0f);
        uvSize = glm::vec2(1.0f);
        return;
    }
    
    const auto& frame = m_currentAnimation->getFrame(m_currentFrame);
    uvOffset = frame.uvOffset;
    uvSize = frame.uvSize;
    
    // Применяем флип к UV координатам, если нужно
    if (m_flipX) {
        uvOffset.x += uvSize.x;
        uvSize.x = -uvSize.x;
    }
    
    if (m_flipY) {
        uvOffset.y += uvSize.y;
        uvSize.y = -uvSize.y;
    }
}

} // namespace rendering