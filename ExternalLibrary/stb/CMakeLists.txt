# external/stb/CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(stb LANGUAGES C CXX)

# ================ ОПЦИИ БИБЛИОТЕКИ ================
option(STB_BUILD_TESTS "Build STB tests" OFF)
option(STB_INSTALL "Generate installation targets" OFF)

# ================ ВЕРСИЯ БИБЛИОТЕКИ ================
set(STB_VERSION "0.1.0")
set(STB_SOVERSION "0")

# ================ ОПРЕДЕЛЕНИЕ ФАЙЛОВ ================
# Все заголовочные файлы STB
set(STB_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/stb.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_image.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_image_write.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_image_resize.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_truetype.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_rect_pack.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_vorbis.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_hexwave.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_perlin.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_sprintf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_leakcheck.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_include.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_ds.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_divide.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_connected_components.h
    CACHE INTERNAL "STB headers"
)

# Исходные файлы (те, которые могут компилироваться)
set(STB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_vorbis.c
    CACHE INTERNAL "STB sources"
)

# ================ СОЗДАНИЕ БИБЛИОТЕКИ ================
# Для STB лучше создать интерфейсную библиотеку (только заголовки)
add_library(stb INTERFACE)

# Добавляем include директорию
target_include_directories(stb INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/stb>
)

# Устанавливаем свойства для экспорта
set_target_properties(stb PROPERTIES
    EXPORT_NAME stb
    VERSION ${STB_VERSION}
    SOVERSION ${STB_SOVERSION}
)

# ================ ОПЦИИ КОМПИЛЯЦИИ ================
# Если нужно определить реализации
option(STB_DEFINE_IMPLEMENTATIONS "Define implementations in the library" OFF)

if(STB_DEFINE_IMPLEMENTATIONS)
    # Создаем обычную статическую библиотеку с реализациями
    add_library(stb_static STATIC ${STB_SOURCES})
    
    target_compile_definitions(stb_static PRIVATE
        STB_IMAGE_IMPLEMENTATION
        STB_IMAGE_WRITE_IMPLEMENTATION
        STB_IMAGE_RESIZE_IMPLEMENTATION
        STB_TRUETYPE_IMPLEMENTATION
        STB_RECT_PACK_IMPLEMENTATION
        STB_VORBIS_IMPLEMENTATION
        STB_PERLIN_IMPLEMENTATION
        STB_SPRINTF_IMPLEMENTATION
        STB_DS_IMPLEMENTATION
    )
    
    target_include_directories(stb_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    
    # Линкуем статическую библиотеку к интерфейсной
    target_link_libraries(stb INTERFACE stb_static)
endif()

# ================ КОМПИЛЯТОРНЫЕ ФЛАГИ ================
if(MSVC)
    target_compile_options(stb INTERFACE /W3)
else()
    target_compile_options(stb INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ================ ТЕСТЫ (опционально) ================
if(STB_BUILD_TESTS AND NOT STB_DEFINE_IMPLEMENTATIONS)
    enable_testing()
    
    # Простой тест для stb_image
    add_executable(stb_image_test tests/test_image.c)
    target_include_directories(stb_image_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(stb_image_test PRIVATE STB_IMAGE_IMPLEMENTATION)
    target_link_libraries(stb_image_test PRIVATE stb)
    add_test(NAME stb_image_test COMMAND stb_image_test)
    
    # Простой тест для stb_image_write
    add_executable(stb_image_write_test tests/test_image_write.c)
    target_include_directories(stb_image_write_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(stb_image_write_test PRIVATE STB_IMAGE_WRITE_IMPLEMENTATION)
    target_link_libraries(stb_image_write_test PRIVATE stb)
    add_test(NAME stb_image_write_test COMMAND stb_image_write_test)
endif()

# ================ УСТАНОВКА ================
if(STB_INSTALL)
    # Устанавливаем заголовки
    install(FILES ${STB_HEADERS} DESTINATION include/stb)
    
    # Устанавливаем цель
    install(TARGETS stb
        EXPORT stbTargets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
        INCLUDES DESTINATION include
    )
    
    if(STB_DEFINE_IMPLEMENTATIONS)
        install(TARGETS stb_static
            EXPORT stbTargets
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
            RUNTIME DESTINATION bin
        )
    endif()
    
    # Экспортируем цели
    install(EXPORT stbTargets
        FILE stbTargets.cmake
        NAMESPACE stb::
        DESTINATION lib/cmake/stb
    )
    
    # Создаем конфигурационный файл
    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/stbConfig.cmake
        INSTALL_DESTINATION lib/cmake/stb
    )
    
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/stbConfigVersion.cmake
        VERSION ${STB_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/stbConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/stbConfigVersion.cmake
        DESTINATION lib/cmake/stb
    )
endif()
