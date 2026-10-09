cmake_minimum_required(VERSION 3.25)
project(AssetSystem VERSION 0.1.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/FluxModule.cmake")

# ==============================================================================
# AssetSystem — загрузка ассетов на стороне процессора (без GPU, окна и рендера).
#
# Виртуальная файловая система (память, каталог, пак), разбор PNG/JPG (stb_image) и glTF (cgltf),
# собственный бинарный формат сетки .fmesh, cooker (исходники → пак), менеджер с кешем и фоновой
# загрузкой на JobSystem. Клиент превращает данные в GPU-ресурсы сам; серверу рендер не нужен.
# Зависимости: JobSystem, stb (private), cgltf (private).
# ==============================================================================
flux_module_option(ASSETSYSTEM_BUILD_TESTS      "Собирать тесты AssetSystem")
flux_module_option(ASSETSYSTEM_BUILD_BENCHMARKS "Собирать бенчмарки AssetSystem")
flux_module_option(ASSETSYSTEM_BUILD_EXAMPLES   "Собирать примеры AssetSystem")
option(ASSETSYSTEM_BUILD_DOCS "Цель AssetSystemDocs (нужен Doxygen)" ON)
set(ASSETSYSTEM_SANITIZE "" CACHE STRING "Санитайзер для тестов AssetSystem: пусто, address, thread или undefined")

# Санитайзер охватывает и задачи (иначе TSan не видит синхронизацию JobSystem).
if(ASSETSYSTEM_SANITIZE AND NOT JOBSYSTEM_SANITIZE)
    set(JOBSYSTEM_SANITIZE "${ASSETSYSTEM_SANITIZE}" CACHE STRING "" FORCE)
endif()

flux_module(MemorySystem)
flux_module(JobSystem)
flux_external(stb)
flux_external(cgltf)

add_library(AssetSystemOptions INTERFACE)
target_compile_features(AssetSystemOptions INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(AssetSystemOptions INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(AssetSystemOptions INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()
if(ASSETSYSTEM_SANITIZE AND NOT MSVC)
    target_compile_options(AssetSystemOptions INTERFACE -fsanitize=${ASSETSYSTEM_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(AssetSystemOptions INTERFACE -fsanitize=${ASSETSYSTEM_SANITIZE})
endif()

add_library(AssetSystem STATIC
    src/code/Path.cpp
    src/code/Vfs.cpp
    src/code/ImageLoader.cpp
    src/code/GltfLoader.cpp
    src/code/MeshBinary.cpp
    src/code/Cook.cpp
    src/code/AssetManager.cpp
)
add_library(engine::AssetSystem ALIAS AssetSystem)

target_include_directories(AssetSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(AssetSystem PUBLIC cxx_std_23)
target_link_libraries(AssetSystem
    PUBLIC  engine::JobSystem
    PRIVATE stb cgltf::cgltf $<BUILD_INTERFACE:AssetSystemOptions>
)

# Cooker: flux_cook <каталог> <выход.fxpk>
add_executable(flux_cook tools/cook.cpp)
target_link_libraries(flux_cook PRIVATE engine::AssetSystem AssetSystemOptions)

if(ASSETSYSTEM_BUILD_TESTS)
    enable_testing()
    flux_external(doctest)
    add_subdirectory(tests)
endif()

if(ASSETSYSTEM_BUILD_BENCHMARKS)
    flux_external(benchmark)
    add_subdirectory(benchmarks)
endif()

if(ASSETSYSTEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ASSETSYSTEM_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(MODULE_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs")
        configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
        add_custom_target(AssetSystemDocs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Генерация документации AssetSystem (Doxygen)"
            VERBATIM
        )
    endif()
endif()
