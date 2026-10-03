# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(MATH_EXAMPLES
    01_fixed_and_world
    02_determinism_tools
)
foreach(example IN LISTS MATH_EXAMPLES)
    set(target MathExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::Math MathOptions)
    if(MATH_BUILD_TESTS)
        add_test(NAME Math.example.${example} COMMAND ${target})
        set_tests_properties(Math.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
