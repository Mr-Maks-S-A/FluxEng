cmake_minimum_required(VERSION 3.25)
project( EventSystem LANGUAGES CXX)

# ==============================================================================
# Header-Only Target Definition
# ==============================================================================
add_library( EventSystem INTERFACE)
add_library( engine::EventSystem ALIAS EventSystem)

# Инклюды для самой библиотеки и всех, кто к ней линкуется
target_include_directories(EventSystem INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(EventSystem INTERFACE
    fmt::fmt
    spdlog::spdlog
)

# Требуем C++23 для подсистемы событий (std::print, std::span, flat_map и т.д.)
target_compile_features(EventSystem INTERFACE cxx_std_23)

# ==============================================================================
# Тесты и Бенчмарки
# ==============================================================================
option(EVENTSYSTEM_BUILD_TESTS "Build EventSystem unit tests" ON)
option(EVENTSYSTEM_BUILD_BENCHMARKS "Build EventSystem benchmarks" ON)

if(EVENTSYSTEM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(EVENTSYSTEM_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()