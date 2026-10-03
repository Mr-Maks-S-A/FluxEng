file(GLOB Phases_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(PhasesTests ${Phases_TEST_SOURCES})
target_link_libraries(PhasesTests PRIVATE engine::Phases PhasesOptions doctest::doctest)
flux_doctest_discover(PhasesTests Phases)
