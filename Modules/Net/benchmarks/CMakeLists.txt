add_executable(NetBenchmarks benchmark_main.cpp)
target_link_libraries(NetBenchmarks PRIVATE engine::Net NetOptions benchmark::benchmark)
