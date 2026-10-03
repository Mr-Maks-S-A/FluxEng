file(GLOB Runes_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(RunesTests ${Runes_TEST_SOURCES})
target_link_libraries(RunesTests PRIVATE engine::Runes RunesOptions doctest::doctest)
flux_doctest_discover(RunesTests Runes)
