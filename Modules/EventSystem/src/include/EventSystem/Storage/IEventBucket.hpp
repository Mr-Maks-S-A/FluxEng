#pragma once
#include <cstddef>

namespace EventSystem::Storage {

class IEventBucket {
public:
    virtual ~IEventBucket() = default;
    
    virtual void clear() = 0;
    virtual void reserve(std::size_t capacity) = 0;
    
    [[nodiscard]] virtual std::size_t size() const = 0;
    [[nodiscard]] virtual std::size_t capacity() const = 0;
    [[nodiscard]] virtual std::size_t allocated_bytes() const = 0;
};

} // namespace EventSystem::Storage
