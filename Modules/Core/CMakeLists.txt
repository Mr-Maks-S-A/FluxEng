cmake_minimum_required(VERSION 3.25)
project(Core VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Core — слой приложения движка.
#
# Владеет окном, рендером и шиной событий, крутит цикл «кадры + фиксированный тик»,
# превращает ввод в события модуля "Platform" и даёт отладку (оверлей шины, F1, граф).
# Игра реализует Core::Game и запускается через Core::run<G>().
#
# Зависимости: EventSystem, RendererSystem, WindowSystem, GLFW (ввод), stb (скриншот).
# ==============================================================================
flux_module_option(CORE_BUILD_TESTS "Собирать тесты Core")

flux_module(EventSystem)
flux_module(WindowSystem)
flux_module(RendererSystem)
flux_external(glfw)
flux_external(stb)

add_library(Core STATIC
    src/code/App.cpp
)
add_library(engine::Core ALIAS Core)

target_include_directories(Core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
# Заголовок WindowSystem пока не чист под -Wshadow/-Wpedantic — подключаем как системный.
target_include_directories(Core SYSTEM PUBLIC
    $<TARGET_PROPERTY:engine::WindowSystem,INTERFACE_INCLUDE_DIRECTORIES>
)

target_compile_features(Core PUBLIC cxx_std_23)
target_link_libraries(Core
    PUBLIC  engine::EventSystem engine::RendererSystem engine::WindowSystem glfw
    PRIVATE stb
)

if(MSVC)
    target_compile_options(Core PRIVATE /W4 /utf-8)
else()
    target_compile_options(Core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# ==============================================================================
# Тесты: то, что не требует окна (FixedStep, разбор аргументов)
# ==============================================================================
if(CORE_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()
