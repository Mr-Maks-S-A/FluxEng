cmake_minimum_required(VERSION 3.25)
project(RendererSystem VERSION 0.1.0 LANGUAGES CXX)

# ==============================================================================
# Опции модуля
# ==============================================================================
option(RENDERERSYSTEM_BUILD_TESTS      "Собирать тесты RendererSystem"            ON)
option(RENDERERSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки RendererSystem"        ON)
option(RENDERERSYSTEM_BUILD_EXAMPLES   "Собирать примеры RendererSystem"          ON)
option(RENDERERSYSTEM_BUILD_DOCS       "Цель RendererSystemDocs (нужен Doxygen)"  ON)
option(RENDERERSYSTEM_SANITIZE         "Собирать RendererSystem с ASan и UBSan"   OFF)

# ==============================================================================
# Общие флаги для всех целей модуля
# ==============================================================================
add_library(RendererSystemOptions INTERFACE)
target_compile_features(RendererSystemOptions INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(RendererSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(RendererSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

if(RENDERERSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(RendererSystemOptions INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(RendererSystemOptions INTERFACE -fsanitize=address,undefined)
endif()

# Заголовки сторонних библиотек подключаются как системные: строгие предупреждения
# модуля относятся только к нашему коду.
function(renderersystem_system_includes target)
    foreach(dependency IN LISTS ARGN)
        target_include_directories(${target} SYSTEM PRIVATE
            $<TARGET_PROPERTY:${dependency},INTERFACE_INCLUDE_DIRECTORIES>)
    endforeach()
endfunction()

# ==============================================================================
# Библиотека
#
# Зависимости: только glm, glad и stb. Окно и GLFW библиотеке не нужны —
# ей достаточно текущего OpenGL-контекста.
# ==============================================================================
add_library(RendererSystem STATIC
    src/code/Animation.cpp
    src/code/Camera2D.cpp
    src/code/Image.cpp
    src/code/Renderer2D.cpp
    src/code/SpriteBatch.cpp
    src/code/GL/Framebuffer.cpp
    src/code/GL/Shader.cpp
    src/code/GL/Texture.cpp
)
add_library(engine::RendererSystem ALIAS RendererSystem)

target_include_directories(RendererSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(RendererSystem PUBLIC cxx_std_23)
target_link_libraries(RendererSystem
    PUBLIC  glm::glm glad
    PRIVATE stb $<BUILD_INTERFACE:RendererSystemOptions>
)
renderersystem_system_includes(RendererSystem glm::glm glad stb)

# ==============================================================================
# Тесты, бенчмарки, примеры, документация
# ==============================================================================
if(RENDERERSYSTEM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(RENDERERSYSTEM_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(RENDERERSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(RENDERERSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(RENDERERSYSTEM_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(RendererSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации RendererSystem (Doxygen)"
            VERBATIM
        )
    else()
        message(STATUS "RendererSystem: Doxygen не найден, цель RendererSystemDocs недоступна")
    endif()
endif()
