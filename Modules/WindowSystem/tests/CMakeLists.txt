# Тесты headless-окна, подписок и перевода кодов GLFW работают без дисплея. Тесты окна GLFW создают скрытое окно;
# если дисплея нет, они пропускаются с сообщением, а не падают.
add_executable(WindowSystemTests
    test_main.cpp
    test_listeners.cpp
    test_headless.cpp
    test_glfw_keymap.cpp
    test_window.cpp
)
target_link_libraries(WindowSystemTests PRIVATE engine::WindowSystem WindowSystemOptions doctest::doctest glad glfw)
target_include_directories(WindowSystemTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/code) # GlfwKeyMap.hpp — внутренний
flux_doctest_discover(WindowSystemTests WindowSystem)
