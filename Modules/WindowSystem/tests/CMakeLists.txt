# ==============================================================================
# Target: WindowSystemTests
# ==============================================================================
add_executable(WindowSystemTests
    main.cpp
)

target_link_libraries(WindowSystemTests PRIVATE
    engine::WindowSystem
    doctest::doctest
)

# Каждый TEST_CASE становится отдельным тестом CTest (тесты открывают окно — нужен дисплей).
flux_doctest_discover(WindowSystemTests WindowSystem)
