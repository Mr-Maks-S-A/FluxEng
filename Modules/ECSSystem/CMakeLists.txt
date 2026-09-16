cmake_minimum_required(VERSION 3.25)
project( ECSSystem LANGUAGES CXX)

# ==============================================================================
# Header-Only Target Definition
# ==============================================================================
add_library( ECSSystem INTERFACE)
add_library( engine::ECSSystem ALIAS ECSSystem)

# Инклюды для самой библиотеки и всех, кто к ней линкуется
target_include_directories(ECSSystem INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ECSSystem INTERFACE
    fmt::fmt
    spdlog::spdlog
)

# Требуем C++23 для подсистемы событий (std::print, std::span, flat_map и т.д.)
target_compile_features(ECSSystem INTERFACE cxx_std_23)

# ==============================================================================
# Тесты и Бенчмарки
# ==============================================================================
option(ECSSYSTEM_BUILD_TESTS "Build ECSSystem unit tests" ON)
option(ECSSYSTEM_BUILD_BENCHMARKS "Build ECSSystem benchmarks" ON)

if(ECSSYSTEM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ECSSYSTEM_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()