# Каждый пример — отдельная программа и одновременно smoke-тест CTest.
set(INPUTSYSTEM_EXAMPLES
    01_actions
    02_record_and_replay
    03_commands
)
foreach(example IN LISTS INPUTSYSTEM_EXAMPLES)
    set(target InputSystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::InputSystem InputSystemOptions)
    if(INPUTSYSTEM_BUILD_TESTS)
        add_test(NAME InputSystem.example.${example} COMMAND ${target})
        set_tests_properties(InputSystem.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
