# ==============================================================================
# Target: WindowSystemTests
# ==============================================================================

# 1. Объявление бинарника тестов
add_executable(WindowSystemTests
    main.cpp
)

# 2. Линковка зависимостей (Модуль + Фреймворк тестирования)
target_link_libraries(WindowSystemTests PRIVATE
    engine::WindowSystem
    doctest::doctest
)

# 3. Наследование флагов C++ (C++20/C++23 из глобального проекта)
target_compile_features(WindowSystemTests PRIVATE cxx_std_20)

# ==============================================================================
# CTest Integration
# ==============================================================================

# Пытаемся автоматически зарегистрировать каждый TEST_CASE в CTest через doctest
include(doctest OPTIONAL RESULT_VARIABLE DOCTEST_FOUND)

if(DOCTEST_FOUND)
    # doctest парсит C++ файл и создает индивидуальные CTest-кейсы
    doctest_discover_tests(WindowSystemTests)
else()
    # Фоллбэк: регистрирует весь бинарник как один общий тест
    add_test(NAME WindowSystemTests COMMAND WindowSystemTests)
endif()
