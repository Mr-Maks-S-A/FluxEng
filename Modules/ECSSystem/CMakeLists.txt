cmake_minimum_required(VERSION 3.25)
project(ECSSystem VERSION 0.2.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Опции модуля
# ==============================================================================
flux_module_option(ECSSYSTEM_BUILD_TESTS      "Собирать тесты ECSSystem")
flux_module_option(ECSSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки ECSSystem")
flux_module_option(ECSSYSTEM_BUILD_EXAMPLES   "Собирать примеры ECSSystem")
option(ECSSYSTEM_BUILD_DOCS        "Цель ECSSystemDocs (нужен Doxygen)"     ON)
option(ECSSYSTEM_SANITIZE          "Собирать тесты ECSSystem с ASan и UBSan" OFF)

# Флаги для тестов, бенчмарков и примеров (библиотека header-only).
add_library(ECSSystemOptions INTERFACE)
target_compile_features(ECSSystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(ECSSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(ECSSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()
if(ECSSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(ECSSystemOptions INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ECSSystemOptions INTERFACE -fsanitize=address,undefined)
endif()

# ==============================================================================
# Header-only библиотека. Зависимостей, кроме стандартной библиотеки C++, нет.
# Entity с поколением (Entity{} — «нет сущности»), страничный sparse set,
# выборки View по наименьшему пулу.
# ==============================================================================
add_library( ECSSystem INTERFACE)
add_library( engine::ECSSystem ALIAS ECSSystem)

target_include_directories(ECSSystem INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(ECSSystem INTERFACE cxx_std_23)

# ==============================================================================
# Тесты и бенчмарки
# ==============================================================================
if(ECSSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(ECSSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(ECSSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ECSSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(MODULE_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(ECSSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации ECSSystem (Doxygen)"
            VERBATIM
        )
    else()
        message(STATUS "ECSSystem: Doxygen не найден, цель ECSSystemDocs недоступна")
    endif()
endif()
