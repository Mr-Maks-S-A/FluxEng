# Каждый пример — отдельная программа и одновременно smoke-тест CTest.
set(RUNTIMESYSTEM_EXAMPLES
    01_headless_server
    02_lifecycle_and_errors
)
foreach(example IN LISTS RUNTIMESYSTEM_EXAMPLES)
    set(target RuntimeSystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::RuntimeSystem RuntimeSystemOptions)
    if(RUNTIMESYSTEM_BUILD_TESTS)
        add_test(NAME RuntimeSystem.example.${example} COMMAND ${target})
        set_tests_properties(RuntimeSystem.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
