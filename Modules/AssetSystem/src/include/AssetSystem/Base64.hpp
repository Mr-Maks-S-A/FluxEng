#pragma once
/**
 * @file Base64.hpp
 * @brief Кодирование base64 — для встроенных `data:` буферов glTF (тесты, генерация ассетов в коде).
 * Декодирует сам cgltf при разборе.
 */

#include <cstddef>
#include <span>
#include <string>

namespace AssetSystem {

[[nodiscard]] inline std::string base64_encode(std::span<const std::byte> data) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::size_t left = data.size() - i;
        const unsigned b0 = std::to_integer<unsigned>(data[i]);
        const unsigned b1 = left > 1 ? std::to_integer<unsigned>(data[i + 1]) : 0u;
        const unsigned b2 = left > 2 ? std::to_integer<unsigned>(data[i + 2]) : 0u;
        out += table[b0 >> 2];
        out += table[((b0 & 3u) << 4) | (b1 >> 4)];
        out += left > 1 ? table[((b1 & 15u) << 2) | (b2 >> 6)] : '=';
        out += left > 2 ? table[b2 & 63u] : '=';
    }
    return out;
}

} // namespace AssetSystem
