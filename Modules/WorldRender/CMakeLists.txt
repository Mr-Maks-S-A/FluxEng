cmake_minimum_required(VERSION 3.25)
project(WorldRender VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# WorldRender — отрисовка мира заклинаний: ландшафт SDF, туман маны, отладочные линии, камеры, оверлей; всё относительно камеры.
# ==============================================================================
flux_module_option(WORLDRENDER_BUILD_EXAMPLES "Собирать примеры WorldRender")
flux_module_option(WORLDRENDER_BUILD_TESTS      "Собирать тесты WorldRender")

flux_module(Math)
flux_module(JobSystem)
flux_module(RendererSystem)
flux_external(glm)

add_library(WorldRenderOptions INTERFACE)
target_compile_features(WorldRenderOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(WorldRenderOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(WorldRenderOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(WorldRender STATIC
    src/code/Camera.cpp
    src/code/TerrainView.cpp
    src/code/FogView.cpp
    src/code/DebugDraw.cpp
)
add_library(engine::WorldRender ALIAS WorldRender)
target_include_directories(WorldRender PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(WorldRender PUBLIC cxx_std_23)
target_link_libraries(WorldRender
    PUBLIC  engine::Math engine::JobSystem engine::RendererSystem glm::glm
    PRIVATE $<BUILD_INTERFACE:WorldRenderOptions>
)

# Готовые источники данных для рендера: ландшафт Terrain и поле маны ManaField. Отдельная цель, чтобы сам
# WorldRender от них не зависел (заголовок Adapters/Terrain.hpp — единственное место, где они встречаются).
flux_module(Terrain)
flux_module(ManaField)
add_library(WorldRenderTerrain INTERFACE)
add_library(engine::WorldRenderTerrain ALIAS WorldRenderTerrain)
target_link_libraries(WorldRenderTerrain INTERFACE engine::WorldRender engine::Terrain engine::ManaField)

if(WORLDRENDER_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(WORLDRENDER_BUILD_EXAMPLES)
    flux_module(WindowSystem)
    flux_external(glfw)
    add_subdirectory(examples)
endif()
