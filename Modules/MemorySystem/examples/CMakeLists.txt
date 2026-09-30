# Каждый пример — отдельная программа и одновременно smoke-тест CTest.
set(MEMORYSYSTEM_EXAMPLES
    01_arena_basics
    02_tick_scratch
    03_pool_particles
    04_pmr_containers
)
foreach(example IN LISTS MEMORYSYSTEM_EXAMPLES)
    set(target MemorySystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::MemorySystem MemorySystemOptions)
    if(MEMORYSYSTEM_BUILD_TESTS)
        add_test(NAME MemorySystem.example.${example} COMMAND ${target})
        set_tests_properties(MemorySystem.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
