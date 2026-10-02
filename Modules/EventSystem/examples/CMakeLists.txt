# Каждый пример — отдельная программа. Примеры запускаются и как smoke-тесты CTest,
# поэтому они не устаревают незаметно при изменении API.
set(EVENTSYSTEM_EXAMPLES
    01_basic_stream
    02_soa_voxels
    03_module_graph
    04_runtime_schema
    05_lanes_policies_causes
)

foreach(example IN LISTS EVENTSYSTEM_EXAMPLES)
    set(target EventSystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::EventSystem EventSystemOptions)

    if(EVENTSYSTEM_BUILD_TESTS)
        add_test(NAME EventSystem.example.${example} COMMAND ${target})
        set_tests_properties(EventSystem.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
