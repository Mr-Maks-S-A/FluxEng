#pragma once
#include <EventSystem/Storage/IEventBucket.hpp>
#include <tuple>
#include <vector>
#include <span>
#include <utility>

#if defined(_MSC_VER)
    #define ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define ALWAYS_INLINE __attribute__((always_inline)) inline
#else
    #define ALWAYS_INLINE inline
#endif

namespace EventSystem::Storage {

template<typename... StreamTypes>
class SoAEventBucket final : public IEventBucket {
    static_assert(sizeof...(StreamTypes) > 0, "SoAEventBucket must contain at least one stream type!");

private:
    std::tuple<std::vector<StreamTypes>...> streams;

    template<std::size_t... Is>
    void reserve_impl(std::size_t capacity, std::index_sequence<Is...>) {
        (std::get<Is>(streams).reserve(capacity), ...);
    }

    template<std::size_t... Is>
    void clear_impl(std::index_sequence<Is...>) {
        (std::get<Is>(streams).clear(), ...);
    }

    template<std::size_t... Is>
    std::size_t allocated_bytes_impl(std::index_sequence<Is...>) const {
        return ((std::get<Is>(streams).capacity() * sizeof(StreamTypes)) + ...);
    }

public:
    SoAEventBucket() = default;

    // Идеальная передача аргументов прямо в векторы
    template<typename... Args>
    ALWAYS_INLINE void push(Args&&... args) {
        static_assert(sizeof...(Args) == sizeof...(StreamTypes), 
                      "Number of arguments does not match stream count!");
                      
        auto tuple_refs = std::forward_as_tuple(std::forward<Args>(args)...);
        [this, &tuple_refs]<std::size_t... Is>(std::index_sequence<Is...>) {
            (std::get<Is>(streams).push_back(std::get<Is>(std::move(tuple_refs))), ...);
        }(std::index_sequence_for<StreamTypes...>{});
    }

    void reserve(std::size_t capacity) override {
        reserve_impl(capacity, std::index_sequence_for<StreamTypes...>{});
    }

    void clear() override {
        clear_impl(std::index_sequence_for<StreamTypes...>{});
    }

    [[nodiscard]] std::size_t size() const override {
        return std::get<0>(streams).size();
    }

    [[nodiscard]] std::size_t capacity() const override {
        return std::get<0>(streams).capacity();
    }

    [[nodiscard]] std::size_t allocated_bytes() const override {
        return allocated_bytes_impl(std::index_sequence_for<StreamTypes...>{});
    }

    template<std::size_t Index>
    [[nodiscard]] auto get_stream() const {
        using ElementType = std::tuple_element_t<Index, std::tuple<StreamTypes...>>;
        return std::span<const ElementType>(std::get<Index>(streams));
    }
};

} // namespace EventSystem::Storage