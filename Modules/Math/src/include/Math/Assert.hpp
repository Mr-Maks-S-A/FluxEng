#pragma once
/**
 * @file Assert.hpp
 * @brief FLUX_ASSERT — проверка инвариантов симуляции с хуком: перед abort() вызывается обработчик
 * (Replay::FlightRecorder сбрасывает в файл последние тики).
 */

namespace Math {

using AssertHandler = void (*)(const char* expression, const char* message, const char* file, int line);

/// @brief Ставит обработчик сбоя ассерта (nullptr — снять). Возвращает прежний.
AssertHandler set_assert_handler(AssertHandler handler) noexcept;
/// @brief Вызывает обработчик, печатает сообщение и вызывает std::abort().
[[noreturn]] void assert_failed(const char* expression, const char* message, const char* file, int line) noexcept;

} // namespace Math

#define FLUX_ASSERT(cond, msg) \
    (static_cast<bool>(cond) ? void(0) : ::Math::assert_failed(#cond, (msg), __FILE__, __LINE__))
