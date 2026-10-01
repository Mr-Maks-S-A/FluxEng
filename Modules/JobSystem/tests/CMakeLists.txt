add_executable(JobSystemTests
    test_main.cpp
    test_scheduler.cpp
    test_parallel.cpp
)
target_link_libraries(JobSystemTests PRIVATE engine::JobSystem JobSystemOptions doctest::doctest)
flux_doctest_discover(JobSystemTests JobSystem)
