#include "AnimationManager.hpp"

namespace rendering {

void AnimationManager::addAnimation(const std::string& name, std::unique_ptr<Animation> animation) {
    if (animation) {
        animation->setName(name);
        m_animations[name] = std::move(animation);
    }
}

Animation* AnimationManager::getAnimation(const std::string& name) {
    auto it = m_animations.find(name);
    return (it != m_animations.end()) ? it->second.get() : nullptr;
}

bool AnimationManager::hasAnimation(const std::string& name) const {
    return m_animations.find(name) != m_animations.end();
}

void AnimationManager::removeAnimation(const std::string& name) {
    m_animations.erase(name);
}

void AnimationManager::clear() {
    m_animations.clear();
}

} // namespace rendering