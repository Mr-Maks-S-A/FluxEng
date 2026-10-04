cmake_minimum_required(VERSION 3.25)
project(Net VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Net — сетевой слой lockstep: пакеты с контрольной суммой, транспорт-абстракция, плохая сеть в памяти,
# синхронизация команд, блобы по хешу, сверка именованных хешей состояния.
# ==============================================================================
flux_module_option(NET_BUILD_EXAMPLES   "Собирать примеры Net")
flux_module_option(NET_BUILD_TESTS      "Собирать тесты Net")
flux_module_option(NET_BUILD_BENCHMARKS "Собирать бенчмарки Net")

flux_module(Math)
flux_module(Replay)

add_library(NetOptions INTERFACE)
target_compile_features(NetOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(NetOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(NetOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(Net STATIC
    src/code/Wire.cpp
    src/code/Transport.cpp
    src/code/Lockstep.cpp
)
add_library(engine::Net ALIAS Net)
target_include_directories(Net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(Net PUBLIC cxx_std_23)
target_link_libraries(Net
    PUBLIC  engine::Math engine::Replay
    PRIVATE $<BUILD_INTERFACE:NetOptions>
)

if(NET_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(NET_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(NET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
