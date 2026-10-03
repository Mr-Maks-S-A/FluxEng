file(GLOB ManaField_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(ManaFieldTests ${ManaField_TEST_SOURCES})
target_link_libraries(ManaFieldTests PRIVATE engine::ManaField ManaFieldOptions doctest::doctest)
flux_doctest_discover(ManaFieldTests ManaField)
