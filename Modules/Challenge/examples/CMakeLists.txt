# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(CHALLENGE_EXAMPLES
    01_play_campaign
    02_custom_level
)
foreach(example IN LISTS CHALLENGE_EXAMPLES)
    set(target ChallengeExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::Challenge ChallengeOptions)
    if(CHALLENGE_BUILD_TESTS)
        add_test(NAME Challenge.example.${example} COMMAND ${target})
        set_tests_properties(Challenge.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
