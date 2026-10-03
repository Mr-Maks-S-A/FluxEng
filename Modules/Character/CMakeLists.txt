cmake_minimum_required(VERSION 3.25)
project(Character VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Character — управляемый персонаж: капсула из сфер, ходьба, прыжок, столкновения по SDF, личная мана.
# ==============================================================================
flux_module_option(CHARACTER_BUILD_EXAMPLES "Собирать примеры Character")
flux_module_option(CHARACTER_BUILD_TESTS      "Собирать тесты Character")

flux_module(Math)
flux_module(ECSSystem)

add_library(CharacterOptions INTERFACE)
target_compile_features(CharacterOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(CharacterOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(CharacterOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(Character STATIC
    src/code/Character.cpp
)
add_library(engine::Character ALIAS Character)
target_include_directories(Character PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(Character PUBLIC cxx_std_23)
target_link_libraries(Character
    PUBLIC  engine::Math engine::ECSSystem
    PRIVATE $<BUILD_INTERFACE:CharacterOptions>
)

if(CHARACTER_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    flux_module(Terrain) # только для тестов: движение по настоящему ландшафту
    add_subdirectory(tests)
endif()

if(CHARACTER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
