cmake_minimum_required(VERSION 3.25)
project(Runes VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Runes — язык рун: стековая машина, мана как бюджет исполнения, заклинания как сущности ECS.
# ==============================================================================
flux_module_option(RUNES_BUILD_EXAMPLES "Собирать примеры Runes")
flux_module_option(RUNES_BUILD_TESTS      "Собирать тесты Runes")
flux_module_option(RUNES_BUILD_BENCHMARKS "Собирать бенчмарки Runes")

flux_module(Math)
flux_module(ECSSystem)
flux_module(EventSystem)

add_library(RunesOptions INTERFACE)
target_compile_features(RunesOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(RunesOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(RunesOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(Runes STATIC
    src/code/Diagnostic.cpp
    src/code/Graph.cpp
    src/code/Program.cpp
    src/code/Spells.cpp
    src/code/Wire.cpp
)
add_library(engine::Runes ALIAS Runes)
target_include_directories(Runes PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(Runes PUBLIC cxx_std_23)
target_link_libraries(Runes
    PUBLIC  engine::Math engine::ECSSystem engine::EventSystem
    PRIVATE $<BUILD_INTERFACE:RunesOptions>
)

if(RUNES_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(RUNES_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(RUNES_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
