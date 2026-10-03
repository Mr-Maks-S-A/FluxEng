# Каждый пример — отдельная программа и smoke-тест CTest (проверяет сам себя, ненулевой код при ошибке).
# 01 — без окна; 02 открывает скрытое окно и крутит 120 тиков.
add_executable(CoreExample_01_actions 01_actions.cpp)
target_link_libraries(CoreExample_01_actions PRIVATE engine::Core)

add_executable(CoreExample_02_minimal_game 02_minimal_game.cpp)
target_link_libraries(CoreExample_02_minimal_game PRIVATE engine::Core)

if(CORE_BUILD_TESTS)
    add_test(NAME Core.example.01_actions COMMAND CoreExample_01_actions)
    set_tests_properties(Core.example.01_actions PROPERTIES LABELS "example")
    add_test(NAME Core.example.02_minimal_game COMMAND CoreExample_02_minimal_game)
    set_tests_properties(Core.example.02_minimal_game PROPERTIES LABELS "example;gpu" TIMEOUT 60)
endif()
