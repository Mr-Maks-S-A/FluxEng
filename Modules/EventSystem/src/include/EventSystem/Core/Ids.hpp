#pragma once
/**
 * @file Ids.hpp
 * @brief Строго типизированные идентификаторы: EventId, ModuleId и счётчик тиков Tick.
 */

#include <EventSystem/Core/FNV1a.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string_view>

namespace EventSystem {

/**
 * @brief Номер тика симуляции.
 *
 * Шина считает время в тиках симуляции, а не в кадрах рендера:
 * пауза и ускорение игры меняют частоту тиков, но не семантику событий.
 */
using Tick = std::uint64_t;

/**
 * @brief Ссылка на конкретное событие: канал, момент, когда оно стало видно, и номер в этом моменте.
 *
 * Нужна дереву причин (ChannelConfig::trace): событие-следствие хранит ссылку на событие-причину.
 * Вычисляется из положения события, поэтому одинакова при любом числе потоков (никаких счётчиков).
 * Нулевая ссылка — «причины нет» (ZII).
 */
struct EventRef {
    std::uint64_t value = 0; ///< (время + 1) << 32 | канал << 20 | номер.

    /// @brief Ссылка на событие `index` канала `channel`, видимое в момент `time` (тик или кадр домена канала).
    [[nodiscard]] static constexpr EventRef make(Tick time, std::uint32_t channel, std::uint32_t index) noexcept {
        return EventRef{((time + 1) << 32) | (std::uint64_t{channel & 0xFFFu} << 20) | (index & 0xFFFFFu)};
    }
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    [[nodiscard]] constexpr Tick time() const noexcept { return (value >> 32) - 1; }
    [[nodiscard]] constexpr std::uint32_t channel() const noexcept { return static_cast<std::uint32_t>(value >> 20) & 0xFFFu; }
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return static_cast<std::uint32_t>(value) & 0xFFFFFu; }

    friend constexpr bool operator==(EventRef, EventRef) noexcept = default;
    friend constexpr auto operator<=>(EventRef, EventRef) noexcept = default;
};

/**
 * @brief Детерминированный идентификатор типа события.
 *
 * Строится как FNV-1a от имени события (`E::event_name`), а не от имени C++-типа.
 * Поэтому переименование структуры или перенос её в другой namespace не меняет
 * идентификатор, и он пригоден для сохранений, сети и реплеев.
 *
 * Это отдельный тип, а не голый `uint64_t`, чтобы его нельзя было перепутать
 * с другими числами (индексами, ID сущностей и т.п.).
 */
struct EventId {
    std::uint64_t value = 0; ///< Значение хеша.

    friend constexpr bool operator==(EventId, EventId) noexcept = default;
    friend constexpr auto operator<=>(EventId, EventId) noexcept = default;
};

/**
 * @brief Вычисляет EventId по имени события.
 * @param name Имя события, например `"physics.collision"`.
 */
[[nodiscard]] constexpr EventId make_event_id(std::string_view name) noexcept {
    return EventId{FNV1a::hash(name)};
}

/**
 * @brief Идентификатор модуля, объявленного в ModuleRegistry.
 *
 * Это индекс в порядке объявления, поэтому он стабилен в пределах одного запуска
 * при одинаковом порядке инициализации модулей.
 */
struct ModuleId {
    /// @brief Значение «модуль не задан».
    static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index = invalid_index; ///< Индекс модуля в ModuleRegistry.

    /// @brief `true`, если идентификатор указывает на модуль.
    [[nodiscard]] constexpr bool valid() const noexcept { return index != invalid_index; }

    friend constexpr bool operator==(ModuleId, ModuleId) noexcept = default;
    friend constexpr auto operator<=>(ModuleId, ModuleId) noexcept = default;
};

} // namespace EventSystem

/// @brief Хеш для использования EventId в неупорядоченных контейнерах.
template<>
struct std::hash<EventSystem::EventId> {
    [[nodiscard]] std::size_t operator()(EventSystem::EventId id) const noexcept {
        return static_cast<std::size_t>(id.value);
    }
};

/// @brief Хеш для использования ModuleId в неупорядоченных контейнерах.
template<>
struct std::hash<EventSystem::ModuleId> {
    [[nodiscard]] std::size_t operator()(EventSystem::ModuleId id) const noexcept {
        return static_cast<std::size_t>(id.index);
    }
};
