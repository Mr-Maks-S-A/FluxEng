add_executable(RuntimeSystemTests
    test_main.cpp
    test_modules.cpp
    test_runtime.cpp
)
target_link_libraries(RuntimeSystemTests PRIVATE engine::RuntimeSystem RuntimeSystemOptions doctest::doctest)
flux_doctest_discover(RuntimeSystemTests RuntimeSystem)
