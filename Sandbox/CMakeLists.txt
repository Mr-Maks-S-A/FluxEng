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
#   AssetLab     — путь ассета: glTF/PNG/JSON → cook → пак → VFS → менеджер с фоновой загрузкой → GPU (модуль движка в клиенте)
#   NetSim       — без окна: несколько узлов «смотрят» на одну детерминированную симуляцию через плохую сеть (lockstep)
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

foreach(game IN ITEMS FallingSand Colony Ecosystem Swarm Siege Voxel CardDuel RuneCell AssetLab)
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

# AssetLab: те же ассеты из пака и из исходного каталога (--raw); оба режима обязаны загрузить сетку, текстуру и данные,
# а отсутствующий файл — завершиться ошибкой-состоянием, не исключением.
target_link_libraries(AssetLab PRIVATE engine::AssetSystem)
if(BUILD_TESTING)
    set_tests_properties(Sandbox.AssetLab PROPERTIES PASS_REGULAR_EXPRESSION "asset_lab: mesh=1 texture=1 spells=1 failed_as_expected=1")
    add_test(NAME Sandbox.AssetLab.raw COMMAND AssetLab --raw --ticks 300)
    set_tests_properties(Sandbox.AssetLab.raw PROPERTIES LABELS "gpu;sandbox" TIMEOUT 60
                         PASS_REGULAR_EXPRESSION "asset_lab: mesh=1 texture=1 spells=1 failed_as_expected=1")
    add_test(NAME Sandbox.AssetLab.threads0 COMMAND AssetLab --threads 0 --ticks 300)
    set_tests_properties(Sandbox.AssetLab.threads0 PROPERTIES LABELS "gpu;sandbox" TIMEOUT 60
                         PASS_REGULAR_EXPRESSION "asset_lab: mesh=1 texture=1 spells=1 failed_as_expected=1")
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

# ------------------------------------------------------------------------------
# NetSim — без окна и GPU: узлы lockstep (звезда или mesh) на RuntimeSystem + NetSystem + ECS + JobSystem.
# Проверяет детерминизм по каждому тику при потерях, джиттере, дубликатах, дрейфе часов и разном числе потоков.
# Тесты не требуют дисплея (метка без gpu).
# ------------------------------------------------------------------------------
add_executable(NetSim netsim/main.cpp)
target_link_libraries(NetSim PRIVATE engine::RuntimeSystem engine::NetSystem engine::ECSSystem SandboxWarnings)

if(BUILD_TESTING)
    set(NETSIM_COMMON --ticks 600)
    add_test(NAME Sandbox.NetSim.star COMMAND NetSim --mode star ${NETSIM_COMMON})
    add_test(NAME Sandbox.NetSim.mesh COMMAND NetSim --mode mesh ${NETSIM_COMMON})
    # Плохая сеть и большая команда: потери 25%, сильный джиттер, дубликаты, 8 игроков.
    add_test(NAME Sandbox.NetSim.hostile COMMAND NetSim --mode star --players 8 --loss 0.25 --jitter-ms 60 --dup 0.1 --seed 7 ${NETSIM_COMMON})
    add_test(NAME Sandbox.NetSim.hostile_mesh COMMAND NetSim --mode mesh --players 6 --loss 0.25 --jitter-ms 60 --dup 0.1 --seed 11 ${NETSIM_COMMON})
    # Разная скорость часов узлов и разное число потоков на узле.
    add_test(NAME Sandbox.NetSim.drift_threads COMMAND NetSim --mode mesh --drift --threads 0,1,3,7 --seed 5 ${NETSIM_COMMON})
    # Внедрённая ошибка: тест проходит, только если протокол её нашёл.
    add_test(NAME Sandbox.NetSim.desync_star COMMAND NetSim --mode star --corrupt-peer 2 --corrupt-tick 100 --expect-desync ${NETSIM_COMMON})
    add_test(NAME Sandbox.NetSim.desync_mesh COMMAND NetSim --mode mesh --corrupt-peer 1 --corrupt-tick 250 --expect-desync ${NETSIM_COMMON})
    set_tests_properties(
        Sandbox.NetSim.star Sandbox.NetSim.mesh Sandbox.NetSim.hostile Sandbox.NetSim.hostile_mesh Sandbox.NetSim.drift_threads
        Sandbox.NetSim.desync_star Sandbox.NetSim.desync_mesh
        PROPERTIES LABELS "sandbox;netsim" TIMEOUT 120)
endif()
