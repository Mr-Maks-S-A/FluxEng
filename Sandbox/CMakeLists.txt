# ==============================================================================
# Sandbox — игры на движке FluxEng: проверка модулей в настоящем игровом коде.
#
#   FallingSand  — клеточная симуляция (массовые SoA-события, эффекты)
#   Colony       — упрощённый RimWorld (цепочки событий между модулями, атлас, анимации)
#   Ecosystem    — трава, зайцы, лисы (рождения и смерти, ссылки с поколением, модули-структуры)
#   Swarm        — стенд производительности: 50 000+ агентов, сетка в памяти тика, замеры систем
#   Siege        — оборона замка на всех модулях: параллельные системы (JobSystem), детерминизм при любом числе потоков
#   Voxel        — примитивный клон Minecraft: 3D, чанки, разведка того, что выделить в модули
#   CardDuel     — 3D-карточная дуэль в духе Hearthstone: все модули и сторонние библиотеки сразу
#   RuneCell     — заклинание как живая клетка: руны-атомы с валентностью, мембрана, деление, обмен маны
#
# Все игры рисуют через RHI: `--backend gl|vulkan` выбирает графический API.
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

# Правила CardDuel — отдельная библиотека без графики: её используют игра и общие тесты (Tests/integration).
add_library(CardDuelRules STATIC card_duel/Rules.cpp)
target_include_directories(CardDuelRules PUBLIC card_duel)
target_link_libraries(CardDuelRules PUBLIC engine::EventSystem engine::JobSystem engine::MemorySystem PRIVATE SandboxWarnings)

foreach(game IN ITEMS FallingSand Colony Ecosystem Swarm Siege Voxel CardDuel RuneCell)
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

# CardDuel: правила, хроника в консоли (spdlog). Второй smoke-тест — ИИ против ИИ без пауз показа.
target_link_libraries(CardDuel PRIVATE CardDuelRules spdlog::spdlog)
if(BUILD_TESTING)
    add_test(NAME Sandbox.CardDuel.autoplay COMMAND CardDuel --autoplay --fast --rollouts 24 --ticks 2400)
    set_tests_properties(Sandbox.CardDuel.autoplay PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120)
    # Бот ввода кликает мышью через Window::inject_*: проверяет выбор лучом и путь клика до команды правилам.
    add_test(NAME Sandbox.CardDuel.input COMMAND CardDuel --input-bot --fast --rollouts 16 --ticks 1800)
    set_tests_properties(Sandbox.CardDuel.input PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120
                         PASS_REGULAR_EXPRESSION "player input commands: [1-9]")
    add_test(NAME Sandbox.RuneCell.autoplay COMMAND RuneCell --autoplay --ticks 2400)
    set_tests_properties(Sandbox.RuneCell.autoplay PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120)
endif()

# Те же игры на Vulkan (если RendererSystem собран с Vulkan-бэкендом): окно без OpenGL-контекста, слои валидации.
# Тест падает, если валидация хоть что-то сообщила.
if(BUILD_TESTING)
    foreach(game IN ITEMS Voxel CardDuel RuneCell)
        set(vulkan_args --backend vulkan --validation --ticks 300)
        if(game STREQUAL "Voxel")
            list(APPEND vulkan_args --autopilot)
        else()
            list(APPEND vulkan_args --autoplay)
        endif()
        add_test(NAME Sandbox.${game}.vulkan COMMAND ${game} ${vulkan_args})
        set_tests_properties(Sandbox.${game}.vulkan PROPERTIES LABELS "gpu;sandbox;vulkan" TIMEOUT 120
                             FAIL_REGULAR_EXPRESSION "vulkan validation|Vulkan SDK or shaderc not found")
    endforeach()
endif()
