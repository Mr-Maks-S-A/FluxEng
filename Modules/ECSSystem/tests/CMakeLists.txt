add_executable(ECSSystemTests
    main_test.cpp
)

target_link_libraries(ECSSystemTests PRIVATE
    engine::ECSSystem
    doctest::doctest
)

target_compile_features(ECSSystemTests PRIVATE cxx_std_23)

include(doctest OPTIONAL RESULT_VARIABLE DOCTEST_INCLUDED)

if(DOCTEST_INCLUDED)
    doctest_discover_tests(ECSSystemTests)
else()
    add_test(NAME ECSSystemTests COMMAND ECSSystemTests)
endif()