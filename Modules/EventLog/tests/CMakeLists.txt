file(GLOB EventLog_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(EventLogTests ${EventLog_TEST_SOURCES})
target_link_libraries(EventLogTests PRIVATE engine::EventLog EventLogOptions doctest::doctest)
flux_doctest_discover(EventLogTests EventLog)
