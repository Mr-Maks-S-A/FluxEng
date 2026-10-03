cmake_minimum_required(VERSION 3.25)
project(ManaField VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# ManaField — скалярное поле маны: диффузия потоками, draw / inject.
# ==============================================================================
flux_module_option(MANAFIELD_BUILD_EXAMPLES "Собирать примеры ManaField")
flux_module_option(MANAFIELD_BUILD_TESTS      "Собирать тесты ManaField")
flux_module_option(MANAFIELD_BUILD_BENCHMARKS "Собирать бенчмарки ManaField")

flux_module(Math)

add_library(ManaFieldOptions INTERFACE)
target_compile_features(ManaFieldOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(ManaFieldOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(ManaFieldOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(ManaField STATIC
    src/code/ManaField.cpp
)
add_library(engine::ManaField ALIAS ManaField)
target_include_directories(ManaField PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(ManaField PUBLIC cxx_std_23)
target_link_libraries(ManaField
    PUBLIC  engine::Math
    PRIVATE $<BUILD_INTERFACE:ManaFieldOptions>
)

if(MANAFIELD_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(MANAFIELD_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(MANAFIELD_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
