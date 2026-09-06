cmake_minimum_required(VERSION 3.25)
project(event_system LANGUAGES CXX)

# ==============================================================================
# Header-Only Target Definition
# ==============================================================================
add_library(event_system INTERFACE)
add_library(engine::event_system ALIAS event_system)

# Инклюды для самой библиотеки и всех, кто к ней линкуется
target_include_directories(event_system INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Требуем C++23 для подсистемы событий (std::print, std::span, flat_map и т.д.)
target_compile_features(event_system INTERFACE cxx_std_23)

# ==============================================================================
# Tests & Benchmarks (Без внешних зависимостей)
# ==============================================================================
option(EVENT_SYSTEM_BUILD_TESTS "Build event_system unit tests" ON)
option(EVENT_SYSTEM_BUILD_BENCHMARKS "Build event_system benchmarks" ON)

if(EVENT_SYSTEM_BUILD_TESTS OR EVENT_SYSTEM_BUILD_BENCHMARKS)
    enable_testing()
endif()

# 1. Юнит-тесты
if(EVENT_SYSTEM_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_event_system.cpp")
    add_executable(test_event_system tests/test_event_system.cpp)
    target_link_libraries(test_event_system PRIVATE engine::event_system)
    
    add_test(NAME test_event_system COMMAND test_event_system)
endif()

# 2. Бенчмарки
if(EVENT_SYSTEM_BUILD_BENCHMARKS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_event_system.cpp")
    add_executable(bench_event_system benchmarks/bench_event_system.cpp)
    target_link_libraries(bench_event_system PRIVATE engine::event_system)
endif()
