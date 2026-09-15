#pragma once
#include <cstdint>
#include <string_view>

namespace EventSystem::Core {

using TypeId = std::uint64_t;

class FNV1a {
public:
    static consteval TypeId hash(std::string_view str) noexcept {
        constexpr std::uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
        constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

        std::uint64_t hash_value = FNV_OFFSET_BASIS;
        for (char c : str) {
            hash_value ^= static_cast<std::uint8_t>(c);
            hash_value *= FNV_PRIME;
        }
        return hash_value;
    }
};

} // namespace EventSystem::Core
