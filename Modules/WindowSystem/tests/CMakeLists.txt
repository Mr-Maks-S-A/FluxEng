# Тесты ввода и подписок работают без дисплея. Тесты окна создают скрытое окно;
# если дисплея нет, они пропускаются с сообщением, а не падают.
add_executable(WindowSystemTests
    test_main.cpp
    test_input.cpp
    test_listeners.cpp
    test_window.cpp
)
target_link_libraries(WindowSystemTests PRIVATE engine::WindowSystem WindowSystemOptions doctest::doctest)
flux_doctest_discover(WindowSystemTests WindowSystem)
