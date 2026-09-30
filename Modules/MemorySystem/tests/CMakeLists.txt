add_executable(MemorySystemTests
    test_main.cpp
    test_core.cpp
    test_virtual.cpp
    test_arena.cpp
    test_pool.cpp
)
target_link_libraries(MemorySystemTests PRIVATE engine::MemorySystem MemorySystemOptions doctest::doctest)
flux_doctest_discover(MemorySystemTests MemorySystem)
