file(GLOB Challenge_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(ChallengeTests ${Challenge_TEST_SOURCES})
target_link_libraries(ChallengeTests PRIVATE engine::Challenge ChallengeOptions doctest::doctest)
flux_doctest_discover(ChallengeTests Challenge)
