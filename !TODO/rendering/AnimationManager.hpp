#pragma once

#include "Animation.hpp"
#include <unordered_map>
#include <memory>
#include <string>

namespace rendering {

class AnimationManager {
public:
    AnimationManager() = default;
    ~AnimationManager() = default;

    void addAnimation(const std::string& name, std::unique_ptr<Animation> animation);
    Animation* getAnimation(const std::string& name);
    bool hasAnimation(const std::string& name) const;
    void removeAnimation(const std::string& name);
    void clear();

private:
    std::unordered_map<std::string, std::unique_ptr<Animation>> m_animations;
};

} // namespace rendering