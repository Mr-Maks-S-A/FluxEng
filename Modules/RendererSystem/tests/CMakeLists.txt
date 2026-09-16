# ==============================================================================
# Target: RendererSystemTests
# ==============================================================================

# 1. Объявление бинарника тестов
add_executable(RendererSystemTests
    main.cpp
)

# 2. Линковка зависимостей (Модуль RendererSystem + doctest)
target_link_libraries(RendererSystemTests PRIVATE
    engine::RendererSystem
    doctest::doctest
)

# 3. Наследование флагов C++ (C++20)
target_compile_features(RendererSystemTests PRIVATE cxx_std_20)

# ==============================================================================
# CTest Integration
# ==============================================================================

include(doctest OPTIONAL RESULT_VARIABLE DOCTEST_FOUND)

if(DOCTEST_FOUND)
    doctest_discover_tests(RendererSystemTests)
else()
    add_test(NAME RendererSystemTests COMMAND RendererSystemTests)
endif()