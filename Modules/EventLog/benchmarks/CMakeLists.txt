add_executable(EventLogBenchmarks benchmark_main.cpp)
target_link_libraries(EventLogBenchmarks PRIVATE engine::EventLog EventLogOptions benchmark::benchmark)
