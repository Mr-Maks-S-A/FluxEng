# Каждый пример — отдельная программа и smoke-тест CTest (проверяет сам себя, ненулевой код при ошибке).
add_executable(RuneEditorExample_01_edit_and_recover 01_edit_and_recover.cpp)
target_link_libraries(RuneEditorExample_01_edit_and_recover PRIVATE engine::RuneEditor RuneEditorOptions)

if(RUNEEDITOR_BUILD_TESTS)
    add_test(NAME RuneEditor.example.01_edit_and_recover COMMAND RuneEditorExample_01_edit_and_recover)
    set_tests_properties(RuneEditor.example.01_edit_and_recover PROPERTIES LABELS "example")
endif()
