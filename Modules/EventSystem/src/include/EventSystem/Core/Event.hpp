#pragma once
/**
 * @file Event.hpp
 * @brief Описание события: поля, раскладка, концепт Event и compile-time свойства.
 *
 * Событие — это обычная trivially copyable структура, которая сама объявляет
 * имя, список полей и (по желанию) раскладку:
 *
 * @code
 * struct CollisionEvent {
 *     std::uint32_t entity_a;
 *     std::uint32_t entity_b;
 *     Vec2          normal;
 *
 *     static constexpr std::string_view event_name = "physics.collision";
 *     static constexpr EventSystem::Layout layout  = EventSystem::Layout::SoA; // по умолчанию AoS
 *     using fields = EventSystem::Fields<
 *         EventSystem::Field<"entity_a", &CollisionEvent::entity_a>,
 *         EventSystem::Field<"entity_b", &CollisionEvent::entity_b>,
 *         EventSystem::Field<"normal",   &CollisionEvent::normal>>;
 * };
 * @endcode
 *
 * Поле описывается указателем на член, поэтому оно «знает» своё событие:
 * попытка прочитать поле чужого события — ошибка компиляции.
 */

#include <EventSystem/Core/FixedString.hpp>
#include <EventSystem/Core/Ids.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace EventSystem {

/**
 * @brief Раскладка событий в памяти.
 *
 * Аналогия с OpenGL: схема события — это VAO (описание атрибутов),
 * а раскладка решает, будут ли атрибуты interleaved (AoS) или в отдельных буферах (SoA).
 */
enum class Layout : std::uint8_t {
    AoS, ///< Array of Structures: события лежат целиком одно за другим. Выгодно, когда читают все поля.
    SoA, ///< Structure of Arrays: каждое поле в своей колонке. Выгодно при массовом чтении части полей.
};

namespace detail {

/// @brief Разбор указателя на член: владелец и тип значения. Основной шаблон — «не указатель на данные».
template<auto Member>
struct member_pointer_traits {
    static constexpr bool is_data_member = false;
};

/// @brief Специализация для `Value Owner::*`.
template<typename Owner, typename Value, Value Owner::*Member>
struct member_pointer_traits<Member> {
    static constexpr bool is_data_member = !std::is_function_v<Value>;
    using owner_type = Owner;
    using value_type = Value;
};

} // namespace detail

/**
 * @brief Описание одного поля события.
 *
 * @tparam Name   Имя поля: используется в схеме, скриптах, отладке и графе.
 * @tparam Member Указатель на член события, например `&CollisionEvent::normal`.
 */
template<FixedString Name, auto Member>
struct Field {
    static_assert(detail::member_pointer_traits<Member>::is_data_member,
                  "Field<Name, Member>: Member must be a pointer to a data member (&Event::field)");

    /// @brief Тип события, которому принадлежит поле.
    using owner_type = typename detail::member_pointer_traits<Member>::owner_type;
    /// @brief Тип значения поля.
    using value_type = typename detail::member_pointer_traits<Member>::value_type;

    /// @brief Имя поля.
    static constexpr std::string_view name = Name.view();
    /// @brief Указатель на член.
    static constexpr auto member = Member;
};

/**
 * @brief Упорядоченный список полей события.
 *
 * Порядок задаёт порядок колонок в SoA-раскладке и порядок полей в схеме.
 * В список должны входить **все** члены структуры: это проверяется при регистрации
 * (см. validate_schema()), иначе SoA-хранилище молча потеряло бы данные.
 *
 * @tparam Fs Специализации Field.
 */
template<typename... Fs>
struct Fields {
    /// @brief Количество полей.
    static constexpr std::size_t count = sizeof...(Fs);
};

namespace detail {

template<typename F, typename E>
inline constexpr bool field_belongs_to_v = false;

template<FixedString Name, auto Member, typename E>
inline constexpr bool field_belongs_to_v<Field<Name, Member>, E> = [] {
    if constexpr (member_pointer_traits<Member>::is_data_member) {
        return std::is_same_v<typename member_pointer_traits<Member>::owner_type, E>;
    } else {
        return false;
    }
}();

template<typename List, typename E>
inline constexpr bool is_field_list_of_v = false;

template<typename... Fs, typename E>
inline constexpr bool is_field_list_of_v<Fields<Fs...>, E> = (field_belongs_to_v<Fs, E> && ...);

template<auto A, auto B>
consteval bool same_member() noexcept {
    if constexpr (std::is_same_v<decltype(A), decltype(B)>) {
        return A == B;
    } else {
        return false;
    }
}

template<auto Member, typename... Fs>
consteval std::size_t index_of_member() noexcept {
    constexpr bool matches[] = {same_member<Fs::member, Member>()..., false};
    for (std::size_t i = 0; i < sizeof...(Fs); ++i) {
        if (matches[i]) {
            return i;
        }
    }
    return sizeof...(Fs);
}

template<typename List, auto Member>
struct field_index;

template<typename... Fs, auto Member>
struct field_index<Fields<Fs...>, Member> {
    static constexpr std::size_t value = index_of_member<Member, Fs...>();
};

template<typename List, std::size_t I>
struct field_at;

template<typename... Fs, std::size_t I>
struct field_at<Fields<Fs...>, I> {
    using type = std::tuple_element_t<I, std::tuple<Fs...>>;
};

template<typename E>
consteval Layout declared_layout() noexcept {
    if constexpr (requires { { E::layout } -> std::convertible_to<Layout>; }) {
        return E::layout;
    } else {
        return Layout::AoS;
    }
}

} // namespace detail

/**
 * @brief Тип, пригодный как событие шины.
 *
 * Требования:
 * - trivially copyable: события копируются через `memcpy`, их можно сериализовать
 *   и передавать между модулями без конструкторов (строки передаются через ID/хендлы);
 * - default-constructible: нужно для сборки события из SoA-колонок;
 * - `static constexpr std::string_view event_name` — имя, из которого строится EventId;
 * - `using fields = Fields<...>` — поля, принадлежащие именно этому типу.
 *
 * Необязательно: `static constexpr Layout layout` (по умолчанию Layout::AoS).
 */
template<typename E>
concept Event =
    std::is_class_v<E> &&
    std::is_trivially_copyable_v<E> &&
    std::default_initializable<E> &&
    requires {
        { E::event_name } -> std::convertible_to<std::string_view>;
        typename E::fields;
    } &&
    detail::is_field_list_of_v<typename E::fields, E>;

/// @brief Раскладка события `E` (Layout::AoS, если не объявлена).
template<Event E>
inline constexpr Layout event_layout_v = detail::declared_layout<E>();

/// @brief Детерминированный идентификатор события `E`.
template<Event E>
inline constexpr EventId event_id_v = make_event_id(E::event_name);

/// @brief Количество полей события `E`.
template<Event E>
inline constexpr std::size_t event_field_count_v = E::fields::count;

/// @brief `I`-е поле события `E` (специализация Field).
template<Event E, std::size_t I>
using event_field_t = typename detail::field_at<typename E::fields, I>::type;

/// @brief Индекс поля `Member` в списке полей `E`; равен event_field_count_v, если поле не из `E`.
template<Event E, auto Member>
inline constexpr std::size_t field_index_v = detail::field_index<typename E::fields, Member>::value;

/// @brief `true`, если `Member` — поле события `E`.
template<Event E, auto Member>
inline constexpr bool is_field_of_v = field_index_v<E, Member> < event_field_count_v<E>;

/// @brief Тип значения поля `Member`.
template<auto Member>
using member_value_t = typename detail::member_pointer_traits<Member>::value_type;

} // namespace EventSystem
