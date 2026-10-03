file(GLOB RuneEditor_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(RuneEditorTests ${RuneEditor_TEST_SOURCES})
target_link_libraries(RuneEditorTests PRIVATE engine::RuneEditorView RuneEditorOptions doctest::doctest)
flux_doctest_discover(RuneEditorTests RuneEditor)
