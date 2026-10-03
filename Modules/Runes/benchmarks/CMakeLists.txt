add_executable(RunesBenchmarks benchmark_main.cpp)
target_link_libraries(RunesBenchmarks PRIVATE engine::Runes RunesOptions benchmark::benchmark)
