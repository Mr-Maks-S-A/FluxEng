# ==============================================================================
# Sandbox — объединяющий проект: EventSystem + RendererSystem + WindowSystem.
#
#   SandboxCore  — общий каркас (окно, рендер, шина, фиксированный тик, ввод → события)
#   FallingSand  — клеточная симуляция (массовые SoA-события, эффекты)
#   Colony       — упрощённый RimWorld (цепочки событий между модулями, атлас, анимации)
#
# Запуск: bin/FallingSand, bin/Colony. `--frames N` — выйти через N кадров,
# `--screenshot file.png` — сохранить последний кадр (вместе с --frames).
# ==============================================================================

add_library(SandboxCore STATIC common/src/App.cpp)
target_include_directories(SandboxCore PUBLIC common/include)
target_link_libraries(SandboxCore PUBLIC
    engine::EventSystem
    engine::RendererSystem
    engine::WindowSystem
    glfw
    stb
)
target_compile_features(SandboxCore PUBLIC cxx_std_23)

# Прикладной код: строгие предупреждения, но без -Wconversion (игровой код полон int↔float).
if(MSVC)
    target_compile_options(SandboxCore PUBLIC /W4 /utf-8)
else()
    target_compile_options(SandboxCore PUBLIC -Wall -Wextra -Wpedantic -Wshadow)
endif()

# Заголовки сторонних библиотек и WindowSystem — системные, их предупреждения не наши.
foreach(dependency IN ITEMS glfw glm::glm glad stb engine::WindowSystem)
    target_include_directories(SandboxCore SYSTEM PUBLIC $<TARGET_PROPERTY:${dependency},INTERFACE_INCLUDE_DIRECTORIES>)
endforeach()

foreach(game IN ITEMS FallingSand Colony)
    string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" folder "${game}")
    string(TOLOWER "${folder}" folder)
    add_executable(${game} ${folder}/main.cpp)
    target_link_libraries(${game} PRIVATE SandboxCore)

    if(BUILD_TESTING)
        # Smoke-тест с GPU: открыть окно, прожить ~2 секунды, выйти без ошибок.
        add_test(NAME Sandbox.${game} COMMAND ${game} --frames 240)
        set_tests_properties(Sandbox.${game} PROPERTIES LABELS "gpu;sandbox" TIMEOUT 60)
    endif()
endforeach()
