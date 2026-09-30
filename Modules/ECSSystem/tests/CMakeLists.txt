add_executable(ECSSystemTests
    main_test.cpp
)

target_link_libraries(ECSSystemTests PRIVATE
    engine::ECSSystem
    doctest::doctest
)

flux_doctest_discover(ECSSystemTests ECSSystem)
