# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(RUNES_EXAMPLES
    01_text_spell
    02_graph_spell
)
foreach(example IN LISTS RUNES_EXAMPLES)
    set(target RunesExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::Runes RunesOptions)
    if(RUNES_BUILD_TESTS)
        add_test(NAME Runes.example.${example} COMMAND ${target})
        set_tests_properties(Runes.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
