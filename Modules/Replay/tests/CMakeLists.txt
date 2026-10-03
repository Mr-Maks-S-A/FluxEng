file(GLOB Replay_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(ReplayTests ${Replay_TEST_SOURCES})
target_link_libraries(ReplayTests PRIVATE engine::Replay ReplayOptions doctest::doctest)
flux_doctest_discover(ReplayTests Replay)
