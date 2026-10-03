file(GLOB Character_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(CharacterTests ${Character_TEST_SOURCES})
target_link_libraries(CharacterTests PRIVATE engine::Character engine::Terrain CharacterOptions doctest::doctest)
flux_doctest_discover(CharacterTests Character)
