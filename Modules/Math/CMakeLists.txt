cmake_minimum_required(VERSION 3.25)
project(Math VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Math — числа детерминированной симуляции.
#
# Fixed (Q16.16), WorldPos (int64, 1/65536 м), целочисленный sqrt, генератор с сидом,
# хеш состояния (FNV-1a), шум на Fixed, FLUX_ASSERT. Никакого float в вычислениях.
# Зависимостей нет.
# ==============================================================================
flux_module_option(MATH_BUILD_EXAMPLES "Собирать примеры Math")
flux_module_option(MATH_BUILD_TESTS      "Собирать тесты Math")
flux_module_option(MATH_BUILD_BENCHMARKS "Собирать бенчмарки Math")

add_library(MathOptions INTERFACE)
target_compile_features(MathOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(MathOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(MathOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(Math STATIC src/code/Math.cpp)
add_library(engine::Math ALIAS Math)
target_include_directories(Math PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(Math PUBLIC cxx_std_23)
target_link_libraries(Math PRIVATE $<BUILD_INTERFACE:MathOptions>)

if(MATH_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(MATH_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(MATH_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
