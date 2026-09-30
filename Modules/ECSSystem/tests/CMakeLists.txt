add_executable(ECSSystemTests
    test_main.cpp
    test_entity.cpp
    test_pool.cpp
    test_world.cpp
)
target_link_libraries(ECSSystemTests PRIVATE engine::ECSSystem ECSSystemOptions doctest::doctest)
flux_doctest_discover(ECSSystemTests ECSSystem)
