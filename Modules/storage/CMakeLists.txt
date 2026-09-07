cmake_minimum_required(VERSION 3.20)
project(storage CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(storage
    src/reed_solomon.cpp
    src/recoverable_file.cpp
)

target_include_directories(storage PUBLIC include)

# Тесты
add_executable(test_storage tests/test_storage.cpp)
target_link_libraries(test_storage PRIVATE storage)

# Бенчмарки
add_executable(bench_storage benchmarks/bench_storage.cpp)
target_link_libraries(bench_storage PRIVATE storage)
