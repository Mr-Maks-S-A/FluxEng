#pragma once
/**
 * @file FNV1a.hpp
 * @brief 64-битный хеш FNV-1a, из которого строятся детерминированные идентификаторы.
 */

#include <cstdint>
#include <string_view>

namespace EventSystem {

/**
 * @brief 64-битный хеш FNV-1a.
 *
 * Результат зависит только от байтов строки, поэтому он одинаков на любом
 * компиляторе, платформе и в любой сборке (в том числе между разными `.so`/`.dll`).
 * Функция `constexpr`: для событий C++ хеш считается при компиляции,
 * а для событий, описанных в рантайме (скрипты, моды), — при регистрации.
 *
 * @warning FNV-1a не криптостойкий. Коллизии имён событий обнаруживаются
 *          при регистрации в EventBus, а не предотвращаются хешем.
 */
class FNV1a {
public:
    /// @brief Начальное значение (offset basis) FNV-1a 64.
    static constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
    /// @brief Простое число FNV-1a 64.
    static constexpr std::uint64_t prime = 1099511628211ULL;

    /**
     * @brief Хеширует строку.
     * @param str Входная строка (без завершающего нуля).
     * @return 64-битный хеш; для пустой строки равен #offset_basis.
     */
    [[nodiscard]] static constexpr std::uint64_t hash(std::string_view str) noexcept {
        std::uint64_t value = offset_basis;
        for (const char c : str) {
            value ^= static_cast<std::uint8_t>(c);
            value *= prime;
        }
        return value;
    }
};

} // namespace EventSystem
