file(GLOB SpellSim_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(SpellSimTests ${SpellSim_TEST_SOURCES})
target_link_libraries(SpellSimTests PRIVATE engine::SpellSim SpellSimOptions doctest::doctest)
flux_doctest_discover(SpellSimTests SpellSim)
