cmake_minimum_required(VERSION 3.25)
project(RendererSystem LANGUAGES CXX)

# Требуем C++20 или C++23
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Собираем статическую библиотеку
add_library(RendererSystem STATIC
    src/code/OpenGLRenderer.cpp
    src/code/Shader.cpp
    src/code/Texture.cpp
    src/include/RendererSystem/IRenderer.hpp
    src/include/RendererSystem/OpenGLRenderer.hpp
    src/include/RendererSystem/Shader.hpp
    src/include/RendererSystem/Texture.hpp
    src/include/RendererSystem/Vertex.hpp
)

# Алиас для удобного подключения внутри проекта
add_library(engine::RendererSystem ALIAS RendererSystem)

# Настройка путей к заголовочным файлам
target_include_directories(RendererSystem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

# Линковка зависимостей (WindowSystem, GLAD, GLFW и GLM)
target_link_libraries(RendererSystem PUBLIC
    engine::WindowSystem
    glad
    glfw
    glm::glm
    stb
)

# Опция для сборки тестов
option(RENDERERSYSTEM_BUILD_TESTS "Build RendererSystem tests" ON)

if(RENDERERSYSTEM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()