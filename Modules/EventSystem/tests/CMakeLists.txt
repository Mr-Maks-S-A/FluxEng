# Исполняемый файл тестов
add_executable(EventSystemTests
    main_test.cpp
)

# Линковка с вашей библиотекой и doctest
target_link_libraries(EventSystemTests PRIVATE
    engine::EventSystem
    doctest::doctest
)

# Установка C++23 для бинарника тестов
target_compile_features(EventSystemTests PRIVATE cxx_std_23)

# ------------------------------------------------------------------------------
# Интеграция с CTest (doctest с маленькой буквы!)
# ------------------------------------------------------------------------------
include(doctest OPTIONAL RESULT_VARIABLE DOCTEST_INCLUDED)

if(DOCTEST_INCLUDED)
    # Авто-дискавери каждого TEST_CASE
    doctest_discover_tests(EventSystemTests)
else()
    # Запасной вариант (гарантирует регистрацию бинарника целиком)
    add_test(NAME EventSystemTests COMMAND EventSystemTests)
endif()