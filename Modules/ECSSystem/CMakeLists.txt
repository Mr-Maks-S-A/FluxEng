cmake_minimum_required(VERSION 3.25)
project( ECSSystem LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Опции модуля
# ==============================================================================
flux_module_option(ECSSYSTEM_BUILD_TESTS      "Собирать тесты ECSSystem")
flux_module_option(ECSSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки ECSSystem")

# ==============================================================================
# Header-only библиотека. Зависимостей, кроме стандартной библиотеки C++, нет.
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
