#pragma once
/**
 * @file Schema.hpp
 * @brief Рантайм-описание события (EventSchema): имена, типы и смещения полей.
 *
 * Схема — мост между C++-событиями и всем, что не знает C++-типов:
 * скриптами заклинаний, модами, отладочным инспектором, сериализацией.
 * По смыслу это аналог VAO в OpenGL: описание того, как читать байты буфера.
 */

#include <EventSystem/Core/Event.hpp>
#include <EventSystem/Core/Ids.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace EventSystem {

/**
 * @brief Тип значения поля, понятный без C++ (для скриптов и инспектора).
 *
 * Всё, что не является скаляром (структуры, массивы, перечисления), помечается
 * как Opaque: такие поля копируются как байты, но не интерпретируются.
 */
enum class FieldKind : std::uint8_t {
    Bool,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float32,
    Float64,
    Opaque,
};

/// @brief Имя FieldKind для вывода (например `"u32"`).
[[nodiscard]] std::string_view to_string(FieldKind kind) noexcept;

/// @brief Имя Layout для вывода (`"AoS"` / `"SoA"`).
[[nodiscard]] std::string_view to_string(Layout layout) noexcept;

/**
 * @brief Определяет FieldKind для C++-типа.
 * @tparam T Тип значения поля.
 */
template<typename T>
[[nodiscard]] consteval FieldKind field_kind_of() noexcept {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        return FieldKind::Bool;
    } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
        if constexpr (sizeof(U) == 1) return FieldKind::Int8;
        else if constexpr (sizeof(U) == 2) return FieldKind::Int16;
        else if constexpr (sizeof(U) == 4) return FieldKind::Int32;
        else if constexpr (sizeof(U) == 8) return FieldKind::Int64;
        else return FieldKind::Opaque;
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (sizeof(U) == 1) return FieldKind::UInt8;
        else if constexpr (sizeof(U) == 2) return FieldKind::UInt16;
        else if constexpr (sizeof(U) == 4) return FieldKind::UInt32;
        else if constexpr (sizeof(U) == 8) return FieldKind::UInt64;
        else return FieldKind::Opaque;
    } else if constexpr (std::is_same_v<U, float> && sizeof(U) == 4) {
        return FieldKind::Float32;
    } else if constexpr (std::is_same_v<U, double> && sizeof(U) == 8) {
        return FieldKind::Float64;
    } else {
        return FieldKind::Opaque;
    }
}

/**
 * @brief Описание одного поля события.
 */
struct FieldDesc {
    std::string name;       ///< Имя поля.
    FieldKind kind{};       ///< Тип значения.
    std::size_t offset = 0; ///< Смещение от начала события (в AoS-представлении), байт.
    std::size_t size = 0;   ///< Размер значения, байт.
    std::size_t alignment = 1; ///< Выравнивание значения, байт.

    bool operator==(const FieldDesc&) const = default;
};

/**
 * @brief Полное рантайм-описание типа события.
 *
 * Для C++-событий строится автоматически функцией schema_of().
 * Для событий из скриптов и модов может быть собрано вручную
 * и зарегистрировано через EventBus::register_schema().
 */
struct EventSchema {
    std::string name;              ///< Имя события (источник EventId).
    EventId id{};                  ///< make_event_id(name).
    std::size_t size = 0;          ///< Размер события целиком (AoS), байт.
    std::size_t alignment = 1;     ///< Выравнивание события, байт.
    Layout layout = Layout::AoS;   ///< Раскладка в памяти.
    std::vector<FieldDesc> fields; ///< Поля в порядке объявления.

    bool operator==(const EventSchema&) const = default;

    /**
     * @brief Ищет поле по имени.
     * @return Указатель на описание поля или `nullptr`.
     */
    [[nodiscard]] const FieldDesc* find_field(std::string_view field_name) const noexcept;

    /**
     * @brief Индекс поля по имени.
     * @return Индекс или `std::nullopt`, если поля нет.
     */
    [[nodiscard]] std::optional<std::size_t> field_index(std::string_view field_name) const noexcept;
};

/**
 * @brief Проверяет корректность схемы.
 *
 * Правила:
 * - имя не пустое, `id == make_event_id(name)`;
 * - размер и выравнивание события корректны (выравнивание — степень двойки, размер кратен ему);
 * - имена полей не пустые и уникальные;
 * - каждое поле выровнено, помещается в событие и не пересекается с другими;
 * - поля покрывают **все** байты события, кроме неизбежного выравнивающего padding.
 *   Если член структуры забыли указать в `fields`, схема будет отклонена.
 *
 * @return `std::nullopt`, если схема корректна, иначе текст первой найденной ошибки.
 */
[[nodiscard]] std::optional<std::string> validate_schema(const EventSchema& schema);

namespace detail {

template<Event E, typename F>
FieldDesc make_field_desc(const E& probe) {
    using Value = typename F::value_type;
    const auto* base = reinterpret_cast<const std::byte*>(std::addressof(probe));
    const auto* member = reinterpret_cast<const std::byte*>(std::addressof(probe.*F::member));
    return FieldDesc{
        .name = std::string(F::name),
        .kind = field_kind_of<Value>(),
        .offset = static_cast<std::size_t>(member - base),
        .size = sizeof(Value),
        .alignment = alignof(Value),
    };
}

template<Event E, typename... Fs>
EventSchema make_schema(Fields<Fs...> /*fields*/) {
    [[maybe_unused]] const E probe{}; // не используется у событий без полей
    return EventSchema{
        .name = std::string(E::event_name),
        .id = event_id_v<E>,
        .size = sizeof(E),
        .alignment = alignof(E),
        .layout = event_layout_v<E>,
        .fields = {make_field_desc<E, Fs>(probe)...},
    };
}

} // namespace detail

/**
 * @brief Схема C++-события `E`.
 *
 * Строится один раз при первом вызове и дальше возвращается по ссылке.
 * Смещения полей вычисляются по реальному объекту, поэтому совпадают с
 * тем, что сгенерировал компилятор.
 *
 * @tparam E Тип события.
 */
template<Event E>
[[nodiscard]] const EventSchema& schema_of() {
    static const EventSchema schema = detail::make_schema<E>(typename E::fields{});
    return schema;
}

} // namespace EventSystem
