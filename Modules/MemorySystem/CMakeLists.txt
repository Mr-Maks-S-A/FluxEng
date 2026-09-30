cmake_minimum_required(VERSION 3.25)
project(MemorySystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# MemorySystem — аллокаторы движка по принципу ZII (Zero Is Initialization):
# арены, пулы, двойная арена на тик, pmr-адаптер, резерв виртуальной памяти ОС.
# Зависимостей, кроме стандартной библиотеки C++ и API ОС, нет.
# ==============================================================================
flux_module_option(MEMORYSYSTEM_BUILD_TESTS      "Собирать тесты MemorySystem")
flux_module_option(MEMORYSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки MemorySystem")
flux_module_option(MEMORYSYSTEM_BUILD_EXAMPLES   "Собирать примеры MemorySystem")
option(MEMORYSYSTEM_BUILD_DOCS     "Цель MemorySystemDocs (нужен Doxygen)"     ON)
option(MEMORYSYSTEM_SANITIZE       "Собирать MemorySystem с ASan и UBSan"      OFF)

add_library(MemorySystemOptions INTERFACE)
target_compile_features(MemorySystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(MemorySystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(MemorySystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()
if(MEMORYSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(MemorySystemOptions INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(MemorySystemOptions INTERFACE -fsanitize=address,undefined)
endif()

add_library(MemorySystem STATIC
    src/code/Arena.cpp
    src/code/VirtualMemory.cpp
)
add_library(engine::MemorySystem ALIAS MemorySystem)

target_include_directories(MemorySystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(MemorySystem PUBLIC cxx_std_23)
target_link_libraries(MemorySystem PRIVATE $<BUILD_INTERFACE:MemorySystemOptions>)

if(MEMORYSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(MEMORYSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(MEMORYSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MEMORYSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(MODULE_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(MemorySystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации MemorySystem (Doxygen)"
            VERBATIM
        )
    else()
        message(STATUS "MemorySystem: Doxygen не найден, цель MemorySystemDocs недоступна")
    endif()
endif()
