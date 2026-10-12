# Каждый пример — отдельная программа и одновременно smoke-тест CTest.
set(AUDIOSYSTEM_EXAMPLES
    01_mix_and_buses
    02_spatial
    03_sound_events
)
foreach(example IN LISTS AUDIOSYSTEM_EXAMPLES)
    set(target AudioSystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::AudioSystem AudioSystemOptions)
    if(AUDIOSYSTEM_BUILD_TESTS)
        add_test(NAME AudioSystem.example.${example} COMMAND ${target})
        set_tests_properties(AudioSystem.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
