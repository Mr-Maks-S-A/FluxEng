#pragma once
/**
 * @file Error.hpp
 * @brief Тип исключений системы событий.
 */

#include <stdexcept>

namespace EventSystem {

/**
 * @brief Ошибка конфигурации системы событий.
 *
 * Бросается только на «холодных» путях: регистрация событий и модулей,
 * получение писателей и читателей. Горячий путь (`emit`, чтение, `advance_tick`)
 * исключений не бросает; нарушения контрактов там ловит `assert` в debug-сборке.
 */
class EventSystemError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace EventSystem
