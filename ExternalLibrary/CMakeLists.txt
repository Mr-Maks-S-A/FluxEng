cmake_minimum_required(VERSION 3.25)
project(ExternalLibrary CXX)

# ------------------------------------------------------------------------------
# Сторонние библиотеки движка.
#
# Опции каждой библиотеки (что выключить, чтобы не собирать лишнее) заданы
# в cmake/FluxModule.cmake, в flux_external(). Модули, собранные сами по себе,
# подключают библиотеки той же функцией — поэтому настройки одни и те же.
# ------------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/FluxModule.cmake")

foreach(library IN ITEMS doctest fmt spdlog benchmark glad glfw glm stb)
    flux_external(${library})
endforeach()

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
    glm::glm
    stb
)
