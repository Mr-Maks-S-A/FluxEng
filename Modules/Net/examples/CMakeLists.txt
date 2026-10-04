# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(NET_EXAMPLES
    01_two_peers
    02_bad_network_and_desync
)
foreach(example IN LISTS NET_EXAMPLES)
    set(target NetExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::Net NetOptions)
    if(NET_BUILD_TESTS)
        add_test(NAME Net.example.${example} COMMAND ${target})
        set_tests_properties(Net.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
