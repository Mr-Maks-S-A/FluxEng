cmake_minimum_required(VERSION 3.20)
project(
    FixedFloat
    VERSION 0.1.0
    DESCRIPTION "Deterministic Fixed-Point Math Library for Fluxborn Engine"
    LANGUAGES CXX
)


# ------------------------------------------------------------------------------
# 1. УСКОРЕНИЕ СБОРКИ (ccache)
# ------------------------------------------------------------------------------
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
    message(STATUS "ccache обнаружен и подключен для C и C++")
endif()

# ------------------------------------------------------------------------------
# 2. ГЛОБАЛЬНЫЕ НАСТРОЙКИ СТАНДАРТА И ВЫХОДНЫХ ПУТЕЙ
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# Единая директория для скомпилированных файлов (.exe, .so, .dll в bin; .a, .lib в lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)


add_subdirectory("${CMAKE_SOURCE_DIR}/Src")
