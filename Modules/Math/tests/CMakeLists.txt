add_executable(MathTests test_main.cpp test_math.cpp)
target_link_libraries(MathTests PRIVATE engine::Math MathOptions doctest::doctest)
flux_doctest_discover(MathTests Math)
