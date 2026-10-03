add_executable(MathBenchmarks benchmark_main.cpp)
target_link_libraries(MathBenchmarks PRIVATE engine::Math MathOptions benchmark::benchmark)
