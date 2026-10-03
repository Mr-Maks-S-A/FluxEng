add_executable(TerrainBenchmarks benchmark_main.cpp)
target_link_libraries(TerrainBenchmarks PRIVATE engine::Terrain TerrainOptions benchmark::benchmark)
