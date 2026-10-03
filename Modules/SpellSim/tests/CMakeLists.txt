file(GLOB SpellSim_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(SpellSimTests ${SpellSim_TEST_SOURCES})
target_link_libraries(SpellSimTests PRIVATE engine::SpellSim SpellSimOptions doctest::doctest)
# Эталонные записи лежат в репозитории рядом с тестами; при FLUX_UPDATE_GOLDEN=1 тест пишет в этот же каталог.
target_compile_definitions(SpellSimTests PRIVATE SPELLSIM_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
flux_doctest_discover(SpellSimTests SpellSim)
