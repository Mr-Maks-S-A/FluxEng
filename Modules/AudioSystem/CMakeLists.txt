cmake_minimum_required(VERSION 3.25)
project(AudioSystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# AudioSystem — звук без привязки к устройству вывода.
#
# Декодеры WAV и OGG Vorbis, микшер с деревом шин (master → sfx/music/voice/…), голоса с приоритетами и вытеснением,
# 3D-звук (затухание, пан, конус, Доплер, срез высоких), реверберация, ducking, лимитер, звуковые события из текстовых
# данных, офлайн-рендер. Устройство вывода подключается через AudioBackend; движок, тесты и сервер без звуковой карты
# от него не зависят. Зависимости: stb_vorbis (private).
# ==============================================================================
flux_module_option(AUDIOSYSTEM_BUILD_TESTS      "Собирать тесты AudioSystem")
flux_module_option(AUDIOSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки AudioSystem")
flux_module_option(AUDIOSYSTEM_BUILD_EXAMPLES   "Собирать примеры AudioSystem")
option(AUDIOSYSTEM_BUILD_DOCS "Цель AudioSystemDocs (нужен Doxygen)" ON)
set(AUDIOSYSTEM_SANITIZE "" CACHE STRING "Санитайзер для тестов AudioSystem: пусто, address, thread или undefined")

flux_external(stb)

add_library(AudioSystemOptions INTERFACE)
target_compile_features(AudioSystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(AudioSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(AudioSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()
if(AUDIOSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(AudioSystemOptions INTERFACE -fsanitize=${AUDIOSYSTEM_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(AudioSystemOptions INTERFACE -fsanitize=${AUDIOSYSTEM_SANITIZE})
endif()

add_library(AudioSystem STATIC
    src/code/Clip.cpp
    src/code/Ogg.cpp
    src/code/Dsp.cpp
    src/code/Engine.cpp
    src/code/SoundEvents.cpp
    src/code/Backend.cpp
)
add_library(engine::AudioSystem ALIAS AudioSystem)

target_include_directories(AudioSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(AudioSystem PUBLIC cxx_std_23)
target_link_libraries(AudioSystem PRIVATE stb $<BUILD_INTERFACE:AudioSystemOptions>)

if(AUDIOSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(AUDIOSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(AUDIOSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(AUDIOSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(MODULE_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(AudioSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации AudioSystem (Doxygen)"
            VERBATIM
        )
    endif()
endif()
