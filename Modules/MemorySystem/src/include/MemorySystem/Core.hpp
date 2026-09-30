#pragma once
/**
 * @file Core.hpp
 * @brief Базовые определения модуля памяти: размеры, выравнивание, концепт ZeroInitializable.
 *
 * Модуль построен вокруг идеи **ZII — Zero Is Initialization**: объект, все байты которого
 * равны нулю, — это корректный объект в «пустом» состоянии. Отсюда два правила:
 *
 * 1. Аллокаторы модуля всегда отдают **обнулённую** память, и конструктор не нужен.
 * 2. Типы, которые в ней размещаются, должны иметь осмысленное нулевое состояние:
 *    `Handle{}` — «нет объекта», `Vec2{}` — начало координат, счётчик `0` — пусто.
 *
 * Компилятор может проверить только первую половину второго правила (тип тривиален),
 * поэтому смысл нуля — это часть контракта типа, и его стоит описать в комментарии.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace MemorySystem {

/// @brief Кибибайты → байты.
consteval std::size_t KiB(std::size_t n) { return n * 1024u; }
/// @brief Мебибайты → байты.
consteval std::size_t MiB(std::size_t n) { return n * 1024u * 1024u; }
/// @brief Гибибайты → байты.
consteval std::size_t GiB(std::size_t n) { return n * 1024u * 1024u * 1024u; }

/// @brief Выравнивание по умолчанию: подходит любому скалярному типу.
inline constexpr std::size_t default_alignment = alignof(std::max_align_t);

/// @brief `true`, если `value` — степень двойки (0 степенью двойки не считается).
[[nodiscard]] constexpr bool is_power_of_two(std::size_t value) noexcept { return std::has_single_bit(value); }

/**
 * @brief Округляет `value` вверх до кратного `alignment`.
 * @pre `alignment` — степень двойки.
 */
[[nodiscard]] constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

/**
 * @brief Тип, который можно размещать в обнулённой памяти без конструктора.
 *
 * Требования:
 * - trivially copyable: объект — это просто его байты;
 * - trivially destructible: освобождение памяти не должно ничего вызывать;
 * - неабстрактный класс или скаляр.
 *
 * Смысл нулевого состояния компилятор проверить не может — это обязанность автора типа.
 */
template<typename T>
concept ZeroInitializable =
    std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> && !std::is_abstract_v<T> &&
    (std::is_scalar_v<T> || std::is_class_v<T> || std::is_array_v<T>);

/**
 * @brief Начинает жизнь массива объектов `T` в уже заполненной памяти, не меняя байтов.
 *
 * Аналог C++23 `std::start_lifetime_as_array`, который есть ещё не во всех стандартных библиотеках.
 * `memmove` — одна из операций, неявно создающих объекты implicit-lifetime типов, а компилятор
 * убирает сам вызов: это ноль инструкций в Release-сборке.
 *
 * @pre `memory` выровнена под `T` и вмещает `count` объектов.
 */
template<ZeroInitializable T>
[[nodiscard]] T* start_lifetime_as_array(void* memory, std::size_t count) noexcept {
    if (memory == nullptr) {
        return nullptr;
    }
    return std::launder(static_cast<T*>(std::memmove(memory, memory, count * sizeof(T))));
}

} // namespace MemorySystem
