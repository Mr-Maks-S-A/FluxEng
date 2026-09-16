#pragma once

#include <cstdint>
#include <cstddef>
#include <bitset>
#include <limits>

namespace FluxECS {

    using Entity = uint32_t;
    constexpr Entity NULL_ENTITY = 0;
    constexpr Entity INVALID_ENTITY = std::numeric_limits<Entity>::max();

    constexpr size_t MAX_COMPONENTS = 64;
    constexpr size_t MAX_ENTITIES = 10000;

    using ComponentMask = std::bitset<MAX_COMPONENTS>;

    inline size_t getUniqueComponentID() {
        static size_t lastID = 0;
        return lastID++;
    }

    template<typename T>
    inline size_t getComponentTypeID() {
        static size_t typeID = getUniqueComponentID();
        return typeID;
    }

} // namespace FluxECS