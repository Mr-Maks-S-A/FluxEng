cmake_minimum_required(VERSION 3.25)
project(Challenge VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Challenge — игровой цикл поверх симуляции: уровни (цель, пределы, мир), судья (победа/поражение/звёзды),
# безголовый прогон решений. Читает симуляцию, не меняет её.
# ==============================================================================
flux_module_option(CHALLENGE_BUILD_EXAMPLES   "Собирать примеры Challenge")
flux_module_option(CHALLENGE_BUILD_TESTS      "Собирать тесты Challenge")
flux_module_option(CHALLENGE_BUILD_BENCHMARKS "Собирать бенчмарки Challenge")

flux_module(Math)
flux_module(Terrain)
flux_module(Runes)
flux_module(Character)
flux_module(Replay)
flux_module(SpellSim)

add_library(ChallengeOptions INTERFACE)
target_compile_features(ChallengeOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(ChallengeOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(ChallengeOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(Challenge STATIC
    src/code/Campaign.cpp
    src/code/Referee.cpp
    src/code/Play.cpp
    src/code/Solutions.cpp
)
add_library(engine::Challenge ALIAS Challenge)
target_include_directories(Challenge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(Challenge PUBLIC cxx_std_23)
target_link_libraries(Challenge
    PUBLIC  engine::Math engine::Terrain engine::Runes engine::Character engine::Replay engine::SpellSim
    PRIVATE $<BUILD_INTERFACE:ChallengeOptions>
)

if(CHALLENGE_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(CHALLENGE_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(CHALLENGE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
