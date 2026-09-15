cmake_minimum_required(VERSION 3.16)
project(ExternalLibrary CXX)

# ------------------------------------------------------------------------------
# Опции внешних библиотек (отключаем лишнее, чтобы сократить время сборки)
# ------------------------------------------------------------------------------

# doctest
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)

# fmt
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)

# spdlog
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE) # Заставляет spdlog использовать внешний fmt


# benchmark
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

# glfw (отключаем лишнее)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

# ------------------------------------------------------------------------------
# Добавление поддиректорий
# ------------------------------------------------------------------------------
add_subdirectory(doctest)
add_subdirectory(fmt)
add_subdirectory(spdlog)
add_subdirectory(benchmark)
add_subdirectory(glad)
add_subdirectory(glfw)


# ------------------------------------------------------------------------------
# Создание интерфейсной библиотеки-обертки для удобного подключения в проект
# ------------------------------------------------------------------------------
add_library(ExternalLibrary INTERFACE)

target_link_libraries(ExternalLibrary INTERFACE
    doctest::doctest
    fmt::fmt
    spdlog::spdlog
    benchmark::benchmark
    glad
    glfw
)
