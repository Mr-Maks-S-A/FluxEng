#pragma once
#include "flight_recorder.hpp"
#include <print>
#include <cstdlib>

namespace engine::debug {

constexpr bool IS_DEBUG_BUILD = 
#ifdef NDEBUG
    false;
#else
    true;
#endif

template<typename... Args>
inline void engine_assert(
    bool condition, 
    std::string_view msg, 
    std::source_location loc = std::source_location::current()
) {
    if constexpr (!IS_DEBUG_BUILD) return;

    if ([[unlikely]] !condition) {
        std::println(stderr, "[CRASH] Assertion failed at {}:{}: {}", 
                     loc.file_name(), loc.line(), msg);
        
        g_flight_recorder.record("CRASH ASSERTION TRIGGERED", 0, loc);
        g_flight_recorder.dump_to_file("crash_event_dump.log");
        std::abort();
    }
}

} // namespace engine::debug
