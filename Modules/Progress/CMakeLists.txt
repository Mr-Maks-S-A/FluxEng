cmake_minimum_required(VERSION 3.25)
project(Progress VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Progress — путь чародея: опыт, ранги и то, что открывает каждый ранг (руны языка, запас маны).
# ==============================================================================
flux_module_option(PROGRESS_BUILD_EXAMPLES "Собирать примеры Progress")
flux_module_option(PROGRESS_BUILD_TESTS      "Собирать тесты Progress")

flux_module(Math)
flux_module(Runes)
flux_module(EventSystem)
flux_module(ECSSystem)

add_library(ProgressOptions INTERFACE)
target_compile_features(ProgressOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(ProgressOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(ProgressOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(Progress STATIC
    src/code/Progress.cpp
)
add_library(engine::Progress ALIAS Progress)
target_include_directories(Progress PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(Progress PUBLIC cxx_std_23)
target_link_libraries(Progress
    PUBLIC  engine::Math engine::Runes
    PRIVATE $<BUILD_INTERFACE:ProgressOptions>
)

if(PROGRESS_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(PROGRESS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
