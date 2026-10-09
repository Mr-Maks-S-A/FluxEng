cmake_minimum_required(VERSION 3.25)
project(InputSystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# InputSystem — ввод без привязки к платформе.
#
# Собственные стабильные коды клавиш, кнопок мыши и геймпада; события ввода; состояние за кадр;
# ActionMap (действия игры ↔ клавиши, переназначение, сохранение в текст); команда ввода за тик
# (для lockstep и реплеев); запись и воспроизведение ввода. Окно и GLFW не нужны: всё тестируется без дисплея.
# Зависимостей нет: только стандартная библиотека.
# ==============================================================================
flux_module_option(INPUTSYSTEM_BUILD_TESTS      "Собирать тесты InputSystem")
flux_module_option(INPUTSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки InputSystem")
flux_module_option(INPUTSYSTEM_BUILD_EXAMPLES   "Собирать примеры InputSystem")
option(INPUTSYSTEM_BUILD_DOCS "Цель InputSystemDocs (нужен Doxygen)" ON)
set(INPUTSYSTEM_SANITIZE "" CACHE STRING "Санитайзер для тестов InputSystem: пусто, address, thread или undefined")

add_library(InputSystemOptions INTERFACE)
target_compile_features(InputSystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(InputSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(InputSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()
if(INPUTSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(InputSystemOptions INTERFACE -fsanitize=${INPUTSYSTEM_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(InputSystemOptions INTERFACE -fsanitize=${INPUTSYSTEM_SANITIZE})
endif()

add_library(InputSystem STATIC
    src/code/Names.cpp
    src/code/InputState.cpp
    src/code/ActionMap.cpp
    src/code/InputLog.cpp
)
add_library(engine::InputSystem ALIAS InputSystem)

target_include_directories(InputSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(InputSystem PUBLIC cxx_std_23)
target_link_libraries(InputSystem PRIVATE $<BUILD_INTERFACE:InputSystemOptions>)

if(INPUTSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(INPUTSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(INPUTSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(INPUTSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(MODULE_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(InputSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации InputSystem (Doxygen)"
            VERBATIM
        )
    endif()
endif()
