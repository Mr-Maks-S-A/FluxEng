cmake_minimum_required(VERSION 3.25)
project(Core VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Core — слой приложения движка.
#
# Клиент: владеет окном и рендером, крутит цикл кадров поверх RuntimeSystem::Runtime (шина событий, задачи,
# память, фиксированный тик, модули с жизненным циклом), превращает ввод в события модуля "Platform"
# и даёт отладку (оверлей шины, F1, граф). Выделенному серверу Core не нужен — только RuntimeSystem.
# Игра реализует Core::Game и запускается через Core::run<G>().
#
# Зависимости: RuntimeSystem (ядро без графики), MemorySystem (память тика и кадра), JobSystem (потоки для систем игры), EventSystem,
# RendererSystem (2D, 3D, шрифт интерфейса, скриншот в PNG), WindowSystem, GLFW (коды клавиш).
# ==============================================================================
flux_module_option(CORE_BUILD_TESTS "Собирать тесты Core")

flux_module(MemorySystem)
flux_module(JobSystem)
flux_module(EventSystem)
flux_module(RuntimeSystem)
flux_module(WindowSystem)
flux_module(RendererSystem)
flux_external(glfw)

add_library(Core STATIC
    src/code/App.cpp
)
add_library(engine::Core ALIAS Core)

target_include_directories(Core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(Core PUBLIC cxx_std_23)
target_link_libraries(Core
    PUBLIC  engine::RuntimeSystem engine::MemorySystem engine::JobSystem engine::EventSystem engine::RendererSystem engine::WindowSystem glfw
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
