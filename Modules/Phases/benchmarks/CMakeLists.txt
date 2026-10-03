add_executable(PhasesBenchmarks benchmark_main.cpp)
target_link_libraries(PhasesBenchmarks PRIVATE engine::Phases PhasesOptions benchmark::benchmark)
