file(GLOB Terrain_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(TerrainTests ${Terrain_TEST_SOURCES})
target_link_libraries(TerrainTests PRIVATE engine::Terrain TerrainOptions doctest::doctest)
flux_doctest_discover(TerrainTests Terrain)
