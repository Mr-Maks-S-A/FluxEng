# ==============================================================================
# Юнит-тесты (doctest)
# ==============================================================================
add_executable(EventSystemTests
    test_main.cpp
    test_core.cpp
    test_schema.cpp
    test_storage.cpp
    test_channel.cpp
    test_bus.cpp
    test_modules.cpp
)

target_link_libraries(EventSystemTests PRIVATE
    engine::EventSystem
    EventSystemOptions
    doctest::doctest
)

if(DEFINED doctest_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
endif()
include(doctest OPTIONAL RESULT_VARIABLE DOCTEST_INCLUDED)
if(DOCTEST_INCLUDED)
    # Каждый TEST_CASE становится отдельным тестом CTest.
    doctest_discover_tests(EventSystemTests TEST_PREFIX "EventSystem.")
else()
    add_test(NAME EventSystem.unit COMMAND EventSystemTests)
endif()

# ==============================================================================
# Тесты «не должно компилироваться»
#
# Каждый файл из compile_fail/ собирается отдельной целью, исключённой из ALL.
# Тест проходит, только если сборка упала И в выводе компилятора есть ожидаемый
# текст: так проверяется, что ошибка именно та, а не случайная опечатка.
# ==============================================================================
set(EVENTSYSTEM_COMPILE_FAIL_CASES
    "foreign_field|field does not belong to event E"
    "column_on_aos|column\\(\\) requires an SoA event"
    "events_on_soa|events\\(\\) requires an AoS event"
    "non_trivial_event|constraints not satisfied|template constraint failure"
)

foreach(case IN LISTS EVENTSYSTEM_COMPILE_FAIL_CASES)
    string(FIND "${case}" "|" separator)
    string(SUBSTRING "${case}" 0 ${separator} name)
    math(EXPR regex_begin "${separator} + 1")
    string(SUBSTRING "${case}" ${regex_begin} -1 expected_error)

    set(target EventSystemCompileFail_${name})
    add_executable(${target} EXCLUDE_FROM_ALL compile_fail/${name}.cpp)
    target_link_libraries(${target} PRIVATE engine::EventSystem)

    add_test(NAME EventSystem.compile_fail.${name}
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${target} --config $<CONFIG>
    )
    set_tests_properties(EventSystem.compile_fail.${name} PROPERTIES
        PASS_REGULAR_EXPRESSION "${expected_error}"
        RESOURCE_LOCK EventSystemBuildDirectory
        LABELS "compile_fail"
    )
endforeach()
