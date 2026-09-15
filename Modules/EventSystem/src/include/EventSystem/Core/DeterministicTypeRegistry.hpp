#pragma once
#include <EventSystem/Core/FNV1a.hpp>
#include <string_view>

namespace EventSystem::Core {

// Базовый шаблон (неопределен по умолчанию, чтобы вызывать ошибку компиляции, если событие не зарегистрировано)
template<typename T>
struct EventTypeTraits;

class DeterministicTypeRegistry {
public:
    template<typename T>
    static consteval TypeId get_id() noexcept {
        constexpr std::string_view name = EventTypeTraits<T>::name();
        return FNV1a::hash(name);
    }

    template<typename T>
    static consteval std::string_view get_name() noexcept {
        return EventTypeTraits<T>::name();
    }
};

} // namespace EventSystem::Core

// Макрос для быстрой регистрации типов событий
#define REGISTER_EVENT_TYPE(Type) \
    template<> \
    struct EventSystem::Core::EventTypeTraits<Type> { \
        static constexpr std::string_view name() noexcept { \
            return #Type; \
        } \
    };