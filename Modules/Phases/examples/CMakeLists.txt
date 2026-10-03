# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(PHASES_EXAMPLES
    01_schedule
)
foreach(example IN LISTS PHASES_EXAMPLES)
    set(target PhasesExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::Phases PhasesOptions)
    if(PHASES_BUILD_TESTS)
        add_test(NAME Phases.example.${example} COMMAND ${target})
        set_tests_properties(Phases.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
