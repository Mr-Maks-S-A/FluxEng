cmake_minimum_required(VERSION 3.25)
project(WindowSystem VERSION 0.2.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Опции модуля
# ==============================================================================
flux_module_option(WINDOWSYSTEM_BUILD_TESTS      "Собирать тесты WindowSystem")
flux_module_option(WINDOWSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки WindowSystem")
flux_module_option(WINDOWSYSTEM_BUILD_EXAMPLES   "Собирать примеры WindowSystem")
option(WINDOWSYSTEM_BUILD_DOCS     "Цель WindowSystemDocs (нужен Doxygen)"   ON)

add_library(WindowSystemOptions INTERFACE)
target_compile_features(WindowSystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(WindowSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(WindowSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

# ==============================================================================
# Библиотека: окно, OpenGL-контекст, ввод и события поверх GLFW + glad.
# InputState и Listeners не зависят от GLFW и тестируются без дисплея.
# ==============================================================================
flux_external(glfw)
flux_external(glad)

add_library(WindowSystem STATIC
    src/code/Window.cpp
)
add_library(engine::WindowSystem ALIAS WindowSystem)

target_include_directories(WindowSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(WindowSystem PUBLIC cxx_std_23)
target_link_libraries(WindowSystem
    PUBLIC  glad glfw
    PRIVATE $<BUILD_INTERFACE:WindowSystemOptions>
)

if(WINDOWSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(WINDOWSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(WINDOWSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(WINDOWSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(MODULE_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(WindowSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации WindowSystem (Doxygen)"
            VERBATIM
        )
    else()
        message(STATUS "WindowSystem: Doxygen не найден, цель WindowSystemDocs недоступна")
    endif()
endif()
