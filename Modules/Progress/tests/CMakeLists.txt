file(GLOB Progress_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(ProgressTests ${Progress_TEST_SOURCES})
target_link_libraries(ProgressTests PRIVATE engine::Progress ProgressOptions doctest::doctest)
flux_doctest_discover(ProgressTests Progress)
