cmake_minimum_required(VERSION 3.25)
project(WindowSystem LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Опции модуля
# ==============================================================================
flux_module_option(WINDOWSYSTEM_BUILD_TESTS "Собирать тесты WindowSystem")

# ==============================================================================
# Библиотека: окно и OpenGL-контекст поверх GLFW + glad
# ==============================================================================
flux_external(glfw)
flux_external(glad)

add_library(WindowSystem STATIC
    src/code/Window.cpp
    src/include/WindowSystem/Window.hpp
)

# Алиас для удобного подключения внутри проекта
add_library(engine::WindowSystem ALIAS WindowSystem)

target_include_directories(WindowSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

# Стандарт задаётся на цели, а не глобально (CMAKE_CXX_STANDARD): модулю хватает C++20,
# а движок вокруг него может собираться на C++23.
target_compile_features(WindowSystem PUBLIC cxx_std_20)

target_link_libraries(WindowSystem PUBLIC
    glad
    glfw
)

if(WINDOWSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()
