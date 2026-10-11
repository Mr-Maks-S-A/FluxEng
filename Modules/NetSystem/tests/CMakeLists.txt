add_executable(NetSystemTests
    test_main.cpp
    test_bytes.cpp
    test_network.cpp
    test_lockstep.cpp
)
target_link_libraries(NetSystemTests PRIVATE engine::NetSystem NetSystemOptions doctest::doctest)
flux_doctest_discover(NetSystemTests NetSystem)
