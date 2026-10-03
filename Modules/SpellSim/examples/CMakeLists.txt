# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(SPELLSIM_EXAMPLES
    01_headless_simulation
)
foreach(example IN LISTS SPELLSIM_EXAMPLES)
    set(target SpellSimExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::SpellSim SpellSimOptions)
    if(SPELLSIM_BUILD_TESTS)
        add_test(NAME SpellSim.example.${example} COMMAND ${target})
        set_tests_properties(SpellSim.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
