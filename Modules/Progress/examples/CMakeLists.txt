# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(PROGRESS_EXAMPLES
    01_ranks
)
foreach(example IN LISTS PROGRESS_EXAMPLES)
    set(target ProgressExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::Progress ProgressOptions)
    if(PROGRESS_BUILD_TESTS)
        add_test(NAME Progress.example.${example} COMMAND ${target})
        set_tests_properties(Progress.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
