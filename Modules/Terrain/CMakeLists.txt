cmake_minimum_required(VERSION 3.25)
project(Terrain VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Terrain — ландшафт из знакового поля расстояний в чанках 32^3, marching tetrahedra, правки, запросы.
# ==============================================================================
flux_module_option(TERRAIN_BUILD_EXAMPLES "Собирать примеры Terrain")
flux_module_option(TERRAIN_BUILD_TESTS      "Собирать тесты Terrain")
flux_module_option(TERRAIN_BUILD_BENCHMARKS "Собирать бенчмарки Terrain")

flux_module(Math)
flux_module(MemorySystem)

add_library(TerrainOptions INTERFACE)
target_compile_features(TerrainOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(TerrainOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(TerrainOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(Terrain STATIC
    src/code/Terrain.cpp
    src/code/Mesher.cpp
)
add_library(engine::Terrain ALIAS Terrain)
target_include_directories(Terrain PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(Terrain PUBLIC cxx_std_23)
target_link_libraries(Terrain
    PUBLIC  engine::Math engine::MemorySystem
    PRIVATE $<BUILD_INTERFACE:TerrainOptions>
)

if(TERRAIN_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(TERRAIN_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(TERRAIN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
