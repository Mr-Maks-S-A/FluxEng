# Каждый пример — отдельная программа и одновременно smoke-тест CTest.
set(JOBSYSTEM_EXAMPLES
    01_tasks
    02_parallel_for
    03_deterministic_output
)
foreach(example IN LISTS JOBSYSTEM_EXAMPLES)
    set(target JobSystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::JobSystem JobSystemOptions)
    if(JOBSYSTEM_BUILD_TESTS)
        add_test(NAME JobSystem.example.${example} COMMAND ${target})
        set_tests_properties(JobSystem.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
