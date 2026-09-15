#pragma once
#include <source_location>
#include <string_view>
#include <cstddef>

namespace EventSystem::Core {

template<typename T>
static consteval std::string_view raw_type_name() noexcept {
    return std::source_location::current().function_name();
}

template<typename T>
static consteval std::string_view extract_type_name() noexcept {
    constexpr std::string_view function = raw_type_name<T>();

#if defined(__GNUC__) && !defined(__clang__)
    constexpr std::string_view prefix = "[with T = ";
    constexpr std::string_view suffix = ";";
    constexpr std::size_t begin = function.find(prefix);
    static_assert(begin != std::string_view::npos, "Failed to parse GCC type name");
    constexpr std::size_t name_begin = begin + prefix.size();
    constexpr std::size_t end = function.find(suffix, name_begin);
    static_assert(end != std::string_view::npos, "Failed to find GCC type name end");
    return function.substr(name_begin, end - name_begin);

#elif defined(__clang__)
    constexpr std::string_view prefix = "[T = ";
    constexpr std::string_view suffix = "]";
    constexpr std::size_t begin = function.find(prefix);
    static_assert(begin != std::string_view::npos, "Failed to parse Clang type name");
    constexpr std::size_t name_begin = begin + prefix.size();
    constexpr std::size_t end = function.rfind(suffix);
    static_assert(end != std::string_view::npos, "Failed to find Clang type name end");
    return function.substr(name_begin, end - name_begin);

#elif defined(_MSC_VER)
    constexpr std::string_view prefix = "raw_type_name<";
    constexpr std::string_view suffix = ">(void)";
    constexpr std::size_t begin = function.find(prefix);
    static_assert(begin != std::string_view::npos, "Failed to parse MSVC type name");
    constexpr std::size_t name_begin = begin + prefix.size();
    constexpr std::size_t end = function.find(suffix, name_begin);
    static_assert(end != std::string_view::npos, "Failed to find MSVC type name end");
    return function.substr(name_begin, end - name_begin);
#else
    #error "Unsupported compiler"
#endif
}

} // namespace EventSystem::Core
