cmake_minimum_required(VERSION 3.25)
project(NetSystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# NetSystem — сеть для детерминированной мультиплеерной игры.
#
# Сериализация, абстракция транспорта, имитация сети (задержка, джиттер, потери, дубликаты) на виртуальном времени
# и lockstep-протокол: по сети ходят команды ввода, состояние считает каждый узел сам, хеши состояния
# сверяются — так ловится рассинхронизация. Топологии: звезда (сервер-ретранслятор) и mesh (p2p).
# Зависимостей нет: только стандартная библиотека. Сокеты подключаются через ITransport.
# ==============================================================================
flux_module_option(NETSYSTEM_BUILD_TESTS      "Собирать тесты NetSystem")
flux_module_option(NETSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки NetSystem")
flux_module_option(NETSYSTEM_BUILD_EXAMPLES   "Собирать примеры NetSystem")
option(NETSYSTEM_BUILD_DOCS "Цель NetSystemDocs (нужен Doxygen)" ON)
set(NETSYSTEM_SANITIZE "" CACHE STRING "Санитайзер для тестов NetSystem: пусто, address, thread или undefined")

add_library(NetSystemOptions INTERFACE)
target_compile_features(NetSystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(NetSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(NetSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()
if(NETSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(NetSystemOptions INTERFACE -fsanitize=${NETSYSTEM_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(NetSystemOptions INTERFACE -fsanitize=${NETSYSTEM_SANITIZE})
endif()

add_library(NetSystem STATIC src/code/SimulatedNetwork.cpp)
add_library(engine::NetSystem ALIAS NetSystem)

target_include_directories(NetSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(NetSystem PUBLIC cxx_std_23)
target_link_libraries(NetSystem PRIVATE $<BUILD_INTERFACE:NetSystemOptions>)

if(NETSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(NETSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(NETSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(NETSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(MODULE_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(NetSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации NetSystem (Doxygen)"
            VERBATIM
        )
    endif()
endif()
