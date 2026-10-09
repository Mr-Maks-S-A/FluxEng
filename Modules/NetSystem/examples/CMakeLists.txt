# Каждый пример — отдельная программа и одновременно smoke-тест CTest.
set(NETSYSTEM_EXAMPLES
    01_lockstep_star
    02_detect_desync
)
foreach(example IN LISTS NETSYSTEM_EXAMPLES)
    set(target NetSystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::NetSystem NetSystemOptions)
    if(NETSYSTEM_BUILD_TESTS)
        add_test(NAME NetSystem.example.${example} COMMAND ${target})
        set_tests_properties(NetSystem.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
