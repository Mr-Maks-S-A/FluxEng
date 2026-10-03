cmake_minimum_required(VERSION 3.25)
project(Phases VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Phases — расписание фаз тика симуляции: упорядоченный список именованных шагов с замерами.
# ==============================================================================
flux_module_option(PHASES_BUILD_EXAMPLES "Собирать примеры Phases")
flux_module_option(PHASES_BUILD_TESTS      "Собирать тесты Phases")
flux_module_option(PHASES_BUILD_BENCHMARKS "Собирать бенчмарки Phases")

flux_module(Math)

add_library(PhasesOptions INTERFACE)
target_compile_features(PhasesOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(PhasesOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(PhasesOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(Phases STATIC
    src/code/Schedule.cpp
)
add_library(engine::Phases ALIAS Phases)
target_include_directories(Phases PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(Phases PUBLIC cxx_std_23)
target_link_libraries(Phases
    PUBLIC  engine::Math
    PRIVATE $<BUILD_INTERFACE:PhasesOptions>
)

if(PHASES_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(PHASES_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(PHASES_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
