cmake_minimum_required(VERSION 3.25)
project(EventSystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Опции модуля
#
# Тесты, бенчмарки и примеры включены по умолчанию при самостоятельной сборке
# (cmake -S Modules/EventSystem) и в режиме разработчика движка (FLUX_DEVELOPER).
# Зависимостей, кроме стандартной библиотеки C++, у модуля нет.
# ==============================================================================
flux_module_option(EVENTSYSTEM_BUILD_TESTS      "Собирать тесты EventSystem")
flux_module_option(EVENTSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки EventSystem")
flux_module_option(EVENTSYSTEM_BUILD_EXAMPLES   "Собирать примеры EventSystem")
option(EVENTSYSTEM_BUILD_DOCS       "Цель EventSystemDocs (нужен Doxygen)"        ON)
option(EVENTSYSTEM_SANITIZE         "Собирать EventSystem с ASan и UBSan"         OFF)

# ==============================================================================
# Общие флаги для всех целей модуля
# ==============================================================================
add_library(EventSystemOptions INTERFACE)
target_compile_features(EventSystemOptions INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(EventSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(EventSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

if(EVENTSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(EventSystemOptions INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(EventSystemOptions INTERFACE -fsanitize=address,undefined)
endif()

# ==============================================================================
# Библиотека
# ==============================================================================
add_library(EventSystem STATIC
    src/code/ColumnBuffer.cpp
    src/code/EventBuffer.cpp
    src/code/EventBus.cpp
    src/code/EventGraph.cpp
    src/code/ModuleRegistry.cpp
    src/code/Schema.cpp
    src/code/StreamChannel.cpp
)
add_library(engine::EventSystem ALIAS EventSystem)

target_include_directories(EventSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(EventSystem PUBLIC cxx_std_23)
target_link_libraries(EventSystem PRIVATE $<BUILD_INTERFACE:EventSystemOptions>)

# ==============================================================================
# Тесты, бенчмарки, примеры, документация
# ==============================================================================
if(EVENTSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(EVENTSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(EVENTSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(EVENTSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(EVENTSYSTEM_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(EventSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации EventSystem (Doxygen)"
            VERBATIM
        )
    else()
        message(STATUS "EventSystem: Doxygen не найден, цель EventSystemDocs недоступна")
    endif()
endif()
