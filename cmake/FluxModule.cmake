# ==============================================================================
# FluxModule.cmake — общие правила для модулей FluxEng.
#
# Каждый модуль (Modules/<Name>) — самостоятельный CMake-проект:
#
#   cmake -S Modules/EventSystem -B build/EventSystem     # только модуль + его тесты
#   cmake -S .                   -B build                 # весь движок
#
# Чтобы это работало, модуль не полагается на то, что корень движка уже что-то
# подключил. Вместо этого он просит нужное через функции ниже:
#
#   flux_external(glfw)          — сторонняя библиотека из ExternalLibrary/
#   flux_module(EventSystem)     — соседний модуль из Modules/
#   flux_module_option(VAR "…")  — опция тестов/бенчмарков/примеров модуля
#
# Если цель уже есть (её подключил движок или другой модуль), функции ничего не делают.
# ==============================================================================
include_guard(GLOBAL)
cmake_minimum_required(VERSION 3.25) # add_subdirectory(... SYSTEM), PROJECT_IS_TOP_LEVEL

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH FLUX_ROOT_DIR)
set(FLUX_EXTERNAL_DIR "${FLUX_ROOT_DIR}/ExternalLibrary" CACHE PATH "Каталог сторонних библиотек FluxEng")
set(FLUX_MODULES_DIR "${FLUX_ROOT_DIR}/Modules" CACHE PATH "Каталог модулей FluxEng")

# Главная цель каждой внешней библиотеки: по ней проверяем, подключена ли она уже.
# Хранится в глобальных свойствах: include_guard(GLOBAL) подключает файл один раз,
# а обычные переменные не видны в соседних каталогах.
set_property(GLOBAL PROPERTY FLUX_TARGET_doctest   doctest::doctest)
set_property(GLOBAL PROPERTY FLUX_TARGET_benchmark benchmark::benchmark)
set_property(GLOBAL PROPERTY FLUX_TARGET_fmt       fmt::fmt)
set_property(GLOBAL PROPERTY FLUX_TARGET_spdlog    spdlog::spdlog)
set_property(GLOBAL PROPERTY FLUX_TARGET_glfw      glfw)
set_property(GLOBAL PROPERTY FLUX_TARGET_glm       glm::glm)
set_property(GLOBAL PROPERTY FLUX_TARGET_glad      glad)
set_property(GLOBAL PROPERTY FLUX_TARGET_stb       stb)

# ------------------------------------------------------------------------------
# flux_module_option(<VAR> <описание>)
#
# Тесты, бенчмарки и примеры модуля включены по умолчанию, только если:
# - модуль собирается сам по себе (cmake -S Modules/<Name>), или
# - движок собирается в режиме разработчика (FLUX_DEVELOPER, по умолчанию ON в корне).
# Когда FluxEng подключён в чужой проект, чужие тесты не тянутся в его сборку.
# ------------------------------------------------------------------------------
function(flux_module_option var description)
    if(PROJECT_IS_TOP_LEVEL OR FLUX_DEVELOPER)
        set(default ON)
    else()
        set(default OFF)
    endif()
    option(${var} "${description}" ${default})
endfunction()

# ------------------------------------------------------------------------------
# flux_external(<name>)
#
# Подключает библиотеку ExternalLibrary/<name>, если её цели ещё нет.
# Подключение с SYSTEM: предупреждения из чужих заголовков не смешиваются с нашими
# (-Wconversion модулей больше не ругается на glm).
# ------------------------------------------------------------------------------
function(flux_external name)
    get_property(main_target GLOBAL PROPERTY FLUX_TARGET_${name})
    if(NOT main_target)
        message(FATAL_ERROR "flux_external: неизвестная библиотека '${name}'")
    endif()
    if(TARGET ${main_target})
        return()
    endif()

    # Выключаем у сторонних библиотек всё, что нам не нужно.
    if(name STREQUAL "doctest")
        set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
        set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    elseif(name STREQUAL "benchmark")
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    elseif(name STREQUAL "fmt")
        set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
        set(FMT_TEST OFF CACHE BOOL "" FORCE)
    elseif(name STREQUAL "spdlog")
        flux_external(fmt)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
        set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
    elseif(name STREQUAL "glfw")
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    elseif(name STREQUAL "glm")
        set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
    endif()

    set(source_dir "${FLUX_EXTERNAL_DIR}/${name}")
    if(NOT EXISTS "${source_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "flux_external: нет ${source_dir}/CMakeLists.txt. "
            "Подмодули не скачаны? Выполните: git submodule update --init")
    endif()
    add_subdirectory("${source_dir}" "${CMAKE_BINARY_DIR}/_external/${name}" SYSTEM)
endfunction()

# ------------------------------------------------------------------------------
# flux_module(<Name>)
#
# Подключает соседний модуль Modules/<Name>, если цели engine::<Name> ещё нет.
# Так модуль верхнего уровня (например, Core) собирается и сам по себе.
# ------------------------------------------------------------------------------
function(flux_module name)
    if(TARGET engine::${name})
        return()
    endif()
    add_subdirectory("${FLUX_MODULES_DIR}/${name}" "${CMAKE_BINARY_DIR}/_modules/${name}")
endfunction()

# ------------------------------------------------------------------------------
# flux_doctest_discover(<target> <prefix>)
#
# Регистрирует каждый TEST_CASE как отдельный тест CTest, если скрипт doctest доступен.
# ------------------------------------------------------------------------------
function(flux_doctest_discover target prefix)
    if(DEFINED doctest_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
    endif()
    include(doctest OPTIONAL RESULT_VARIABLE doctest_script)
    if(doctest_script)
        doctest_discover_tests(${target} TEST_PREFIX "${prefix}.")
    else()
        add_test(NAME ${prefix}.unit COMMAND ${target})
    endif()
endfunction()
