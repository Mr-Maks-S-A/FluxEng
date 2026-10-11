add_executable(InputSystemTests
    test_main.cpp
    test_keys.cpp
    test_state.cpp
    test_action_map.cpp
    test_command.cpp
    test_log.cpp
)
target_link_libraries(InputSystemTests PRIVATE engine::InputSystem InputSystemOptions doctest::doctest)
flux_doctest_discover(InputSystemTests InputSystem)
