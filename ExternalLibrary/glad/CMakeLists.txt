# external/glad/CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(glad LANGUAGES C)

# Создаем библиотеку
add_library(glad STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/glad.c")

# Настраиваем инклюды
target_include_directories(glad 
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Подавляем предупреждения для glad
target_compile_options(glad 
    PRIVATE 
        $<$<C_COMPILER_ID:MSVC>:/W0>
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>
)

# Алиас
add_library(glad::glad ALIAS glad)

# Сообщаем об успешной конфигурации
# message(STATUS "GLAD library configured: ${CMAKE_CURRENT_SOURCE_DIR}")
