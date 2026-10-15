cmake_minimum_required(VERSION 3.25)
project(AudioSystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# AudioSystem — звук: Wave (PCM, синтез, WAV), Mixer (голоса, ADSR, панорама — чистая функция времени, без платформы)
# и Device (вывод через SDL3). Микшер и WAV работают и тестируются без звуковой карты.
# ==============================================================================
flux_module_option(AUDIOSYSTEM_BUILD_TESTS    "Собирать тесты AudioSystem")

flux_external(sdl)

add_library(AudioSystemOptions INTERFACE)
target_compile_features(AudioSystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(AudioSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(AudioSystemOptions INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_library(AudioSystem STATIC
    src/code/Wave.cpp
    src/code/Mixer.cpp
    src/code/Device.cpp
)
add_library(engine::AudioSystem ALIAS AudioSystem)
target_include_directories(AudioSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(AudioSystem PUBLIC cxx_std_23)
target_link_libraries(AudioSystem PRIVATE SDL3::SDL3 $<BUILD_INTERFACE:AudioSystemOptions>)

if(AUDIOSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()
