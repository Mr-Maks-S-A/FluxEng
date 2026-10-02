cmake_minimum_required(VERSION 3.25)
project(RendererSystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# Опции модуля
#
# Тесты, бенчмарки и примеры включены по умолчанию при самостоятельной сборке
# (cmake -S Modules/RendererSystem) и в режиме разработчика движка (FLUX_DEVELOPER).
# ==============================================================================
flux_module_option(RENDERERSYSTEM_BUILD_TESTS      "Собирать тесты RendererSystem")
flux_module_option(RENDERERSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки RendererSystem")
flux_module_option(RENDERERSYSTEM_BUILD_EXAMPLES   "Собирать примеры RendererSystem")
option(RENDERERSYSTEM_BUILD_DOCS       "Цель RendererSystemDocs (нужен Doxygen)"  ON)
option(RENDERERSYSTEM_SANITIZE         "Собирать RendererSystem с ASan и UBSan"   OFF)

# ==============================================================================
# Общие флаги для всех целей модуля
# ==============================================================================
add_library(RendererSystemOptions INTERFACE)
target_compile_features(RendererSystemOptions INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(RendererSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(RendererSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

if(RENDERERSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(RendererSystemOptions INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(RendererSystemOptions INTERFACE -fsanitize=address,undefined)
endif()

# ==============================================================================
# Библиотека
#
# Зависимости: только glm, glad и stb (stb_image, stb_image_write, stb_truetype,
# stb_rect_pack, stb_easy_font, stb_perlin). Окно и GLFW библиотеке не нужны —
# ей достаточно текущего OpenGL-контекста. Сторонние библиотеки подключены
# как SYSTEM (см. flux_external), их предупреждения не смешиваются с нашими.
# ==============================================================================
flux_external(glm)
flux_external(glad)
flux_external(stb)

add_library(RendererSystem STATIC
    src/code/Animation.cpp
    src/code/Camera2D.cpp
    src/code/Camera3D.cpp
    src/code/Font.cpp
    src/code/Image.cpp
    src/code/MeshData.cpp
    src/code/Procedural.cpp
    src/code/Renderer2D.cpp
    src/code/Renderer3D.cpp
    src/code/SpriteBatch.cpp
    src/code/GL/Shader.cpp
    src/code/RHI/Device.cpp
    src/code/RHI/OpenGLDevice.cpp
    src/code/RHI/Resources.cpp
)
add_library(engine::RendererSystem ALIAS RendererSystem)

# ------------------------------------------------------------------------------
# Vulkan-бэкенд (необязательно): нужны Vulkan SDK (заголовки + загрузчик) и shaderc
# (компиляция GLSL → SPIR-V во время работы). Без них модуль собирается только с OpenGL.
# ------------------------------------------------------------------------------
option(RENDERERSYSTEM_VULKAN "Vulkan-бэкенд RHI (если найдены Vulkan SDK и shaderc)" ON)
if(RENDERERSYSTEM_VULKAN)
    find_package(Vulkan QUIET)
    # shaderc: разделяемая библиотека (Linux-пакеты, Vulkan SDK); статическая «combined» — запасной вариант
    # (в некоторых дистрибутивах она без glslang и не линкуется).
    set(_flux_vulkan_dirs "")
    if(Vulkan_FOUND)
        get_filename_component(_flux_vulkan_dirs "${Vulkan_LIBRARY}" DIRECTORY)
    endif()
    find_library(FLUX_SHADERC_LIBRARY NAMES shaderc_shared shaderc_combined HINTS ${_flux_vulkan_dirs} "$ENV{VULKAN_SDK}/lib")
    find_path(FLUX_SHADERC_INCLUDE shaderc/shaderc.h HINTS ${Vulkan_INCLUDE_DIRS} "$ENV{VULKAN_SDK}/include")
    if(Vulkan_FOUND AND FLUX_SHADERC_LIBRARY AND FLUX_SHADERC_INCLUDE)
        target_sources(RendererSystem PRIVATE src/code/RHI/VulkanDevice.cpp)
        target_compile_definitions(RendererSystem PRIVATE FLUX_RENDERER_VULKAN=1)
        target_include_directories(RendererSystem SYSTEM PRIVATE "${FLUX_SHADERC_INCLUDE}")
        target_link_libraries(RendererSystem PRIVATE Vulkan::Vulkan "${FLUX_SHADERC_LIBRARY}")
        message(STATUS "RendererSystem: Vulkan backend ON (Vulkan ${Vulkan_VERSION}, ${FLUX_SHADERC_LIBRARY})")
    else()
        message(STATUS "RendererSystem: Vulkan SDK or shaderc not found — OpenGL backend only")
    endif()
endif()

target_include_directories(RendererSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(RendererSystem PUBLIC cxx_std_23)
target_link_libraries(RendererSystem
    PUBLIC  glm::glm glad
    PRIVATE stb $<BUILD_INTERFACE:RendererSystemOptions>
)

# ==============================================================================
# Тесты, бенчмарки, примеры, документация
# ==============================================================================
if(RENDERERSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    flux_external(glfw)
    add_subdirectory(tests)
endif()

if(RENDERERSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    flux_external(glfw)
    add_subdirectory(benchmarks)
endif()

if(RENDERERSYSTEM_BUILD_EXAMPLES)
    # Примеры 01 и 02 открывают окно через WindowSystem.
    flux_module(WindowSystem)
    add_subdirectory(examples)
endif()

if(RENDERERSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(RENDERERSYSTEM_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(RendererSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации RendererSystem (Doxygen)"
            VERBATIM
        )
    else()
        message(STATUS "RendererSystem: Doxygen не найден, цель RendererSystemDocs недоступна")
    endif()
endif()
