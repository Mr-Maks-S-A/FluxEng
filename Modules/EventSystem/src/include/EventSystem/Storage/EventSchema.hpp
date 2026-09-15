#pragma once
#include "EventSystem/Core/DeterministicTypeRegistry.hpp"
#include <cstddef>

namespace EventSystem::Storage {

template<typename T>
struct EventSchema {
    using value_type = T;
    static constexpr Core::TypeId type_id = Core::DeterministicTypeRegistry::get_id<T>();
    static constexpr std::size_t size = sizeof(T);
    static constexpr std::size_t alignment = alignof(T);
};

} // namespace EventSystem::Storage
