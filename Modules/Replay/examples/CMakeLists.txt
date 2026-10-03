# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(REPLAY_EXAMPLES
    01_record_and_replay
)
foreach(example IN LISTS REPLAY_EXAMPLES)
    set(target ReplayExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::Replay ReplayOptions)
    if(REPLAY_BUILD_TESTS)
        add_test(NAME Replay.example.${example} COMMAND ${target})
        set_tests_properties(Replay.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
