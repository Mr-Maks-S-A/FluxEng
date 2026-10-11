add_executable(CoreTests
    test_main.cpp
    test_fixed_step.cpp
    test_args.cpp
)

target_link_libraries(CoreTests PRIVATE
    engine::Core
    doctest::doctest
)

flux_doctest_discover(CoreTests Core)
