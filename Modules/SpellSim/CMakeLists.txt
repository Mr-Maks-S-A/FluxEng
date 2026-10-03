cmake_minimum_required(VERSION 3.25)
project(SpellSim VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# SpellSim — симуляция «Первого заклинания»: порядок тика, команды, мир из ландшафта, поля маны, рун и персонажа.
# ==============================================================================
flux_module_option(SPELLSIM_BUILD_EXAMPLES "Собирать примеры SpellSim")
flux_module_option(SPELLSIM_BUILD_TESTS      "Собирать тесты SpellSim")

flux_module(Math)
flux_module(Phases)
flux_module(Replay)
flux_module(Terrain)
flux_module(ManaField)
flux_module(Runes)
flux_module(Character)
flux_module(EventSystem)
flux_module(ECSSystem)

add_library(SpellSimOptions INTERFACE)
target_compile_features(SpellSimOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(SpellSimOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(SpellSimOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(SpellSim STATIC
    src/code/SpellSim.cpp
)
add_library(engine::SpellSim ALIAS SpellSim)
target_include_directories(SpellSim PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(SpellSim PUBLIC cxx_std_23)
target_link_libraries(SpellSim
    PUBLIC  engine::Math engine::Phases engine::Replay engine::Terrain engine::ManaField engine::Runes engine::Character engine::EventSystem engine::ECSSystem
    PRIVATE $<BUILD_INTERFACE:SpellSimOptions>
)

if(SPELLSIM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(SPELLSIM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
