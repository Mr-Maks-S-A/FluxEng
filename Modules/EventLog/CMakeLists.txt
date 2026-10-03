cmake_minimum_required(VERSION 3.25)
project(EventLog VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# EventLog — журнал событий с избыточностью: записи с контрольными суммами, полосы с кодом Рида—Соломона, восстановление после повреждений.
# ==============================================================================
flux_module_option(EVENTLOG_BUILD_EXAMPLES "Собирать примеры EventLog")
flux_module_option(EVENTLOG_BUILD_TESTS      "Собирать тесты EventLog")
flux_module_option(EVENTLOG_BUILD_BENCHMARKS "Собирать бенчмарки EventLog")

flux_module(Math)

add_library(EventLogOptions INTERFACE)
target_compile_features(EventLogOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(EventLogOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(EventLogOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(EventLog STATIC
    src/code/ErasureCode.cpp
    src/code/Storage.cpp
    src/code/Journal.cpp
)
add_library(engine::EventLog ALIAS EventLog)
target_include_directories(EventLog PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(EventLog PUBLIC cxx_std_23)
target_link_libraries(EventLog
    PUBLIC  engine::Math
    PRIVATE $<BUILD_INTERFACE:EventLogOptions>
)

if(EVENTLOG_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(EVENTLOG_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

# Адаптер шины событий: единственное место, знающее про EventSystem (журнал от шины не зависит).
flux_module(EventSystem)
add_library(EventLogBus INTERFACE)
add_library(engine::EventLogBus ALIAS EventLogBus)
target_link_libraries(EventLogBus INTERFACE engine::EventLog engine::EventSystem)

if(EVENTLOG_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
