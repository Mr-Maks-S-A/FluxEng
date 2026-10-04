file(GLOB Net_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(NetTests ${Net_TEST_SOURCES})
target_link_libraries(NetTests PRIVATE engine::Net NetOptions doctest::doctest)
flux_doctest_discover(NetTests Net)
