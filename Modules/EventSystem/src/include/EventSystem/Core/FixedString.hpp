#pragma once
/**
 * @file FixedString.hpp
 * @brief Строка фиксированной длины, пригодная как параметр шаблона.
 */

#include <cstddef>
#include <string_view>

namespace EventSystem {

/**
 * @brief Строковый литерал, который можно передать параметром шаблона.
 *
 * Нужен, чтобы писать `Field<"entity_a", &Event::entity_a>`: имя поля
 * хранится в типе и доступно и при компиляции, и в рантайме (для скриптов и отладки).
 *
 * @tparam N Размер литерала вместе с завершающим нулём.
 */
template<std::size_t N>
struct FixedString {
    char chars[N]{}; ///< Символы литерала, включая завершающий ноль.

    /// @brief Неявное построение из строкового литерала.
    consteval FixedString(const char (&str)[N]) noexcept { // NOLINT(google-explicit-constructor)
        for (std::size_t i = 0; i < N; ++i) {
            chars[i] = str[i];
        }
    }

    /// @brief Представление без завершающего нуля.
    [[nodiscard]] constexpr std::string_view view() const noexcept { return {chars, N - 1}; }
};

} // namespace EventSystem
