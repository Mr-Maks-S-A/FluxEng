# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(CHARACTER_EXAMPLES
    01_walk_on_plane
)
foreach(example IN LISTS CHARACTER_EXAMPLES)
    set(target CharacterExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::Character CharacterOptions)
    if(CHARACTER_BUILD_TESTS)
        add_test(NAME Character.example.${example} COMMAND ${target})
        set_tests_properties(Character.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
