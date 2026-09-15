
cmake_minimum_required(VERSION 3.25)
project(WindowSystem LANGUAGES CXX)

# Требуем C++20 или C++23
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Собираем статическую библиотеку
add_library(WindowSystem STATIC
    src/code/Window.cpp
    src/include/WindowSystem/Window.hpp
)

# Алиас для удобного подключения внутри проекта
add_library(engine::WindowSystem ALIAS WindowSystem)

# Настройка путей к заголовочным файлам
target_include_directories(WindowSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

# Линковка GLFW
target_link_libraries(WindowSystem PUBLIC
    glad
    glfw
)


option(WINDOWSYSTEM_BUILD_TESTS "Build WindowSystem tests" ON)

if(WINDOWSYSTEM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()