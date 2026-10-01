cmake_minimum_required(VERSION 3.25)
project(JobSystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# JobSystem — задачи и параллельные алгоритмы движка.
# Пул потоков с общей очередью; ожидающий поток помогает выполнять задачи;
# parallel_for с детерминированной нарезкой; временная арена на каждый поток.
# Зависимости: MemorySystem (scratch-арены) и std::thread.
# ==============================================================================
flux_module_option(JOBSYSTEM_BUILD_TESTS      "Собирать тесты JobSystem")
flux_module_option(JOBSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки JobSystem")
flux_module_option(JOBSYSTEM_BUILD_EXAMPLES   "Собирать примеры JobSystem")
option(JOBSYSTEM_BUILD_DOCS        "Цель JobSystemDocs (нужен Doxygen)"       ON)
set(JOBSYSTEM_SANITIZE "" CACHE STRING "Санитайзер для тестов JobSystem: пусто, address или thread")

flux_module(MemorySystem)
find_package(Threads REQUIRED)

add_library(JobSystemOptions INTERFACE)
target_compile_features(JobSystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(JobSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(JobSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()
if(JOBSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(JobSystemOptions INTERFACE -fsanitize=${JOBSYSTEM_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(JobSystemOptions INTERFACE -fsanitize=${JOBSYSTEM_SANITIZE})
endif()

add_library(JobSystem STATIC src/code/Scheduler.cpp)
add_library(engine::JobSystem ALIAS JobSystem)

target_include_directories(JobSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(JobSystem PUBLIC cxx_std_23)
target_link_libraries(JobSystem
    PUBLIC  engine::MemorySystem Threads::Threads
    PRIVATE $<BUILD_INTERFACE:JobSystemOptions>
)

if(JOBSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(JOBSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(JOBSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(JOBSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(MODULE_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(JobSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации JobSystem (Doxygen)"
            VERBATIM
        )
    else()
        message(STATUS "JobSystem: Doxygen не найден, цель JobSystemDocs недоступна")
    endif()
endif()
