cmake_minimum_required(VERSION 3.25)
project(Replay VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Replay — запись и повтор команд, журнал последних тиков (FlightRecorder).
# ==============================================================================
flux_module_option(REPLAY_BUILD_EXAMPLES "Собирать примеры Replay")
flux_module_option(REPLAY_BUILD_TESTS      "Собирать тесты Replay")

flux_module(Math)

add_library(ReplayOptions INTERFACE)
target_compile_features(ReplayOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(ReplayOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(ReplayOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(Replay STATIC
    src/code/Replay.cpp
)
add_library(engine::Replay ALIAS Replay)
target_include_directories(Replay PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(Replay PUBLIC cxx_std_23)
target_link_libraries(Replay
    PUBLIC  engine::Math
    PRIVATE $<BUILD_INTERFACE:ReplayOptions>
)

if(REPLAY_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(REPLAY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
