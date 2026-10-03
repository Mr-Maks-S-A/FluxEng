# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(MANAFIELD_EXAMPLES
    01_hole_that_heals
)
foreach(example IN LISTS MANAFIELD_EXAMPLES)
    set(target ManaFieldExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::ManaField ManaFieldOptions)
    if(MANAFIELD_BUILD_TESTS)
        add_test(NAME ManaField.example.${example} COMMAND ${target})
        set_tests_properties(ManaField.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
