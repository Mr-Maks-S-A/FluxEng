# Каждый пример — отдельная программа и одновременно smoke-тест CTest.
set(ECSSYSTEM_EXAMPLES
    01_basics
    02_generations
    03_systems
)
foreach(example IN LISTS ECSSYSTEM_EXAMPLES)
    set(target ECSSystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::ECSSystem ECSSystemOptions)
    if(ECSSYSTEM_BUILD_TESTS)
        add_test(NAME ECSSystem.example.${example} COMMAND ${target})
        set_tests_properties(ECSSystem.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
