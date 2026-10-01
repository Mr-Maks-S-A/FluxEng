# ==============================================================================
# Sandbox — игры на движке FluxEng: проверка модулей в настоящем игровом коде.
#
#   FallingSand  — клеточная симуляция (массовые SoA-события, эффекты)
#   Colony       — упрощённый RimWorld (цепочки событий между модулями, атлас, анимации)
#   Ecosystem    — трава, зайцы, лисы (рождения и смерти, ссылки с поколением, модули-структуры)
#   Swarm        — стенд производительности: 50 000+ агентов, сетка в памяти тика, замеры систем
#   Siege        — оборона замка на всех модулях: параллельные системы (JobSystem), детерминизм при любом числе потоков
#
# Каркас приложения (окно, рендер, шина, фиксированный тик, ввод → события) живёт
# в модуле движка Core (Modules/Core). Игра — это Core::Game и одна строка в main().
#
# Запуск: bin/FallingSand, bin/Colony. `--frames N` — выйти через N кадров,
# `--ticks N` — ровно N тиков по одному на кадр (детерминированно),
# `--screenshot file.png` — сохранить последний кадр.
# ==============================================================================

# Прикладной код: строгие предупреждения, но без -Wconversion (игровой код полон int↔float).
add_library(SandboxWarnings INTERFACE)
if(MSVC)
    target_compile_options(SandboxWarnings INTERFACE /W4 /utf-8)
else()
    target_compile_options(SandboxWarnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
endif()

foreach(game IN ITEMS FallingSand Colony Ecosystem Swarm Siege)
    string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" folder "${game}")
    string(TOLOWER "${folder}" folder)
    add_executable(${game} ${folder}/main.cpp)
    target_link_libraries(${game} PRIVATE engine::Core engine::ECSSystem SandboxWarnings)

    if(BUILD_TESTING)
        # Smoke-тест с GPU: открыть окно, прожить 300 тиков (по одному на кадр), выйти без ошибок.
        add_test(NAME Sandbox.${game} COMMAND ${game} --ticks 300)
        set_tests_properties(Sandbox.${game} PROPERTIES LABELS "gpu;sandbox" TIMEOUT 60)
    endif()
endforeach()
