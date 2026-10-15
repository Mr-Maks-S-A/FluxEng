# Тесты headless-окна, подписок и перевода кодов SDL работают без дисплея. Тесты окна SDL создают скрытое окно;
# если дисплея нет, они пропускаются с сообщением, а не падают.
add_executable(WindowSystemTests
    test_main.cpp
    test_listeners.cpp
    test_headless.cpp
    test_sdl_keymap.cpp
    test_window.cpp
)
target_link_libraries(WindowSystemTests PRIVATE engine::WindowSystem WindowSystemOptions doctest::doctest glad SDL3::SDL3)
target_include_directories(WindowSystemTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/code) # SdlKeyMap.hpp — внутренний
flux_doctest_discover(WindowSystemTests WindowSystem)
