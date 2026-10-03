cmake_minimum_required(VERSION 3.25)
project(RuneEditor VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# RuneEditor — редактор графов рун без GPU: правки как данные, undo/redo, живая диагностика, раскладка, управление мышью, автосохранение в журнал.
# ==============================================================================
flux_module_option(RUNEEDITOR_BUILD_TESTS      "Собирать тесты RuneEditor")
flux_module_option(RUNEEDITOR_BUILD_EXAMPLES   "Собирать примеры RuneEditor")

flux_module(Math)
flux_module(Runes)
flux_module(EventLog)

add_library(RuneEditorOptions INTERFACE)
target_compile_features(RuneEditorOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(RuneEditorOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(RuneEditorOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(RuneEditor STATIC
    src/code/Geometry.cpp
    src/code/Editor.cpp
    src/code/Analysis.cpp
    src/code/Layout.cpp
    src/code/Controller.cpp
    src/code/Autosave.cpp
)
add_library(engine::RuneEditor ALIAS RuneEditor)
target_include_directories(RuneEditor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(RuneEditor PUBLIC cxx_std_23)
target_link_libraries(RuneEditor
    PUBLIC  engine::Math engine::Runes engine::EventLog
    PRIVATE $<BUILD_INTERFACE:RuneEditorOptions>
)

# Отрисовка (Renderer2D): отдельная цель, чтобы модель редактора не зависела от графики.
flux_module(RendererSystem)
add_library(RuneEditorView STATIC src/code/View.cpp)
add_library(engine::RuneEditorView ALIAS RuneEditorView)
target_link_libraries(RuneEditorView PUBLIC engine::RuneEditor engine::RendererSystem PRIVATE $<BUILD_INTERFACE:RuneEditorOptions>)

if(RUNEEDITOR_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(RUNEEDITOR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
