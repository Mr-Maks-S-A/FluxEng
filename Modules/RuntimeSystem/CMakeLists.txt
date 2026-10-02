cmake_minimum_required(VERSION 3.25)
project(RuntimeSystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# RuntimeSystem — ядро игры без графики и окна.
#
# Шина событий, задачи, память тика и кадра, фиксированный тик и модули с жизненным циклом
# (declare → init → frame/tick → shutdown, порядок по зависимостям). Это то, что нужно и клиенту
# (Core::App строится поверх), и выделенному серверу (Runtime::run() без окна).
# Зависимости: MemorySystem, JobSystem, EventSystem. Ни GLFW, ни OpenGL/Vulkan, ни GLM.
# ==============================================================================
flux_module_option(RUNTIMESYSTEM_BUILD_TESTS      "Собирать тесты RuntimeSystem")
flux_module_option(RUNTIMESYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки RuntimeSystem")
flux_module_option(RUNTIMESYSTEM_BUILD_EXAMPLES   "Собирать примеры RuntimeSystem")
option(RUNTIMESYSTEM_BUILD_DOCS "Цель RuntimeSystemDocs (нужен Doxygen)" ON)
set(RUNTIMESYSTEM_SANITIZE "" CACHE STRING "Санитайзер для тестов RuntimeSystem: пусто, address, thread или undefined")

# Санитайзер должен охватывать и задачи: без инструментации JobSystem ThreadSanitizer не видит его
# release/acquire и сообщает о гонках, которых нет.
if(RUNTIMESYSTEM_SANITIZE AND NOT JOBSYSTEM_SANITIZE)
    set(JOBSYSTEM_SANITIZE "${RUNTIMESYSTEM_SANITIZE}" CACHE STRING "" FORCE)
endif()

flux_module(MemorySystem)
flux_module(JobSystem)
flux_module(EventSystem)

add_library(RuntimeSystemOptions INTERFACE)
target_compile_features(RuntimeSystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(RuntimeSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(RuntimeSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()
if(RUNTIMESYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(RuntimeSystemOptions INTERFACE -fsanitize=${RUNTIMESYSTEM_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(RuntimeSystemOptions INTERFACE -fsanitize=${RUNTIMESYSTEM_SANITIZE})
endif()

add_library(RuntimeSystem STATIC src/code/Runtime.cpp)
add_library(engine::RuntimeSystem ALIAS RuntimeSystem)

target_include_directories(RuntimeSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(RuntimeSystem PUBLIC cxx_std_23)
target_link_libraries(RuntimeSystem
    PUBLIC  engine::MemorySystem engine::JobSystem engine::EventSystem
    PRIVATE $<BUILD_INTERFACE:RuntimeSystemOptions>
)

if(RUNTIMESYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(RUNTIMESYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(RUNTIMESYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(RUNTIMESYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(MODULE_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(RuntimeSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации RuntimeSystem (Doxygen)"
            VERBATIM
        )
    endif()
endif()
