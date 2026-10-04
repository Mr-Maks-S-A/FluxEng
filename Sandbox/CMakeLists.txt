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
#   WorldJournal — эксперимент: мир как «сид + журнал правок» (EventLog), снимки, повреждения файла
#   RuneCell     — заклинание как живая клетка: руны-атомы с валентностью, мембрана, деление, обмен маны
#   RuneCell2    — заклинание-граф: вид сверху на SDF-мир, поле маны и визуальный редактор рун (RuneEditor); уровни Challenge (--level)
#   ModuleProof  — игра-проверка: все модули движка в живом приложении, чек-лист на экране (линкуется с engine::FluxEng)
#   CoopScribe   — «пилот и писарь»: два игрока проходят уровень вместе по плохой сети (Net lockstep + SetProgram), консольная
#   FirstSpell   — пре-альфа «Первое заклинание»: персонаж на SDF-ландшафте, руны, мана как поле, запись и повтор
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

foreach(game IN ITEMS FallingSand Colony Ecosystem Swarm Siege Voxel CardDuel RuneCell FirstSpell RuneCell2)
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

# FirstSpell: пре-альфа «Первое заклинание» — вся логика в модулях SpellSim / Replay / WorldRender.
# Игре достаточно одной зонтичной цели (engine::FluxEng = Gameplay + Presentation; см. Modules/CMakeLists.txt).
target_link_libraries(FirstSpell PRIVATE engine::FluxEng)
target_link_libraries(RuneCell2 PRIVATE engine::FluxEng)
target_compile_definitions(RuneCell2 PRIVATE RUNE_CELL2_SPELLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/rune_cell2/spells2")
# WorldJournal: эксперимент «мир как сид + журнал правок» (сохранение через EventLog, снимки изменённых чанков, повреждения файла).
# Консольный: окна и графики нет, нужен только Gameplay (Terrain + EventLog).
add_executable(WorldJournal world_journal/main.cpp)
target_link_libraries(WorldJournal PRIVATE engine::Terrain engine::EventLog engine::Math SandboxWarnings)
if(BUILD_TESTING)
    add_test(NAME Sandbox.WorldJournal COMMAND WorldJournal --edits 900 --snapshot-every 400 --dir "${CMAKE_CURRENT_BINARY_DIR}/world_journal_test")
    set_tests_properties(Sandbox.WorldJournal PROPERTIES LABELS "sandbox" TIMEOUT 120 PASS_REGULAR_EXPRESSION "\nOK\n" FAIL_REGULAR_EXPRESSION "ОШИБКА")
endif()

# ModuleProof: игра-проверка — все модули движка в живом приложении (линкуется с engine::FluxEng): чек-лист на экране и в консоли.
add_executable(ModuleProof module_proof/main.cpp)
target_link_libraries(ModuleProof PRIVATE engine::FluxEng engine::Core SandboxWarnings)
if(BUILD_TESTING)
    add_test(NAME Sandbox.ModuleProof COMMAND ModuleProof --ticks 60 --screenshot module_proof_test.png)
    set_tests_properties(Sandbox.ModuleProof PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120 PASS_REGULAR_EXPRESSION "ModuleProof: OK" FAIL_REGULAR_EXPRESSION "ОШИБКА")
endif()

# CoopScribe: «пилот и писарь» — два игрока проходят уровень вместе по плохой сети (Net lockstep + Challenge + SetProgram).
# Консольный: окна и графики нет, GPU не нужен — идёт в обычном прогоне CTest.
add_executable(CoopScribe coop_scribe/main.cpp)
target_link_libraries(CoopScribe PRIVATE engine::Gameplay SandboxWarnings)
if(BUILD_TESTING)
    foreach(level IN ITEMS mine mound fort)
        add_test(NAME Sandbox.CoopScribe.${level} COMMAND CoopScribe --level ${level})
        set_tests_properties(Sandbox.CoopScribe.${level} PROPERTIES LABELS "sandbox;net" TIMEOUT 120 PASS_REGULAR_EXPRESSION "\nOK\n" FAIL_REGULAR_EXPRESSION "ОШИБКА")
    endforeach()
    add_test(NAME Sandbox.CoopScribe.terrible_network COMMAND CoopScribe --level mound --latency 6 --jitter 8 --loss 300 --dup 150 --corrupt 80 --delay 5)
    set_tests_properties(Sandbox.CoopScribe.terrible_network PROPERTIES LABELS "sandbox;net" TIMEOUT 120 PASS_REGULAR_EXPRESSION "\nOK\n" FAIL_REGULAR_EXPRESSION "ОШИБКА")
    add_test(NAME Sandbox.CoopScribe.cheater COMMAND CoopScribe --level mound --cheat)
    set_tests_properties(Sandbox.CoopScribe.cheater PROPERTIES LABELS "sandbox;net" TIMEOUT 120 PASS_REGULAR_EXPRESSION "OK \\(читера поймали\\)" FAIL_REGULAR_EXPRESSION "ОШИБКА")
endif()

# ReplayTool: просмотр и сравнение записей (файл самоописываем — нужен только Replay).
add_executable(ReplayTool first_spell/replay_tool.cpp)
target_link_libraries(ReplayTool PRIVATE engine::Replay SandboxWarnings)
target_compile_definitions(FirstSpell PRIVATE FIRST_SPELL_SPELLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/first_spell/spells")
if(BUILD_TESTING)
    # Прогон по сценарию без ввода и повтор записи: хеши ландшафта, поля маны и ECS на последнем тике совпадают.
    add_test(NAME Sandbox.FirstSpell.autoplay COMMAND FirstSpell --autoplay --ticks 900)
    set_tests_properties(Sandbox.FirstSpell.autoplay PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120)
    add_test(NAME Sandbox.FirstSpell.record COMMAND FirstSpell --autoplay --ticks 1200 --record first_spell_test.rec)
    set_tests_properties(Sandbox.FirstSpell.record PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120 FIXTURES_SETUP first_spell_record
                         PASS_REGULAR_EXPRESSION "recorded 1200 ticks")
    add_test(NAME Sandbox.FirstSpell.inspect COMMAND ReplayTool inspect first_spell_test.rec 5)
    set_tests_properties(Sandbox.FirstSpell.inspect PROPERTIES LABELS "sandbox" FIXTURES_REQUIRED first_spell_record
                         PASS_REGULAR_EXPRESSION "move dx=1 dz=0")
    add_test(NAME Sandbox.FirstSpell.diff COMMAND ReplayTool diff first_spell_test.rec first_spell_test.rec)
    set_tests_properties(Sandbox.FirstSpell.diff PROPERTIES LABELS "sandbox" FIXTURES_REQUIRED first_spell_record
                         PASS_REGULAR_EXPRESSION "записи совпадают")
    add_test(NAME Sandbox.FirstSpell.replay COMMAND FirstSpell --replay first_spell_test.rec)
    set_tests_properties(Sandbox.FirstSpell.replay PROPERTIES LABELS "gpu;sandbox" TIMEOUT 180 FIXTURES_REQUIRED first_spell_record
                         PASS_REGULAR_EXPRESSION "hashes MATCH the recording" FAIL_REGULAR_EXPRESSION "DIFFER FROM")
endif()

# RuneCell2: сценарий без ввода — граф строится жестами и правками редактора, выстрел, undo/redo, автосохранение с порчей блока;
# запись и повтор (в записи правки редактора идут командами SetProgram и лежат в файле блобами; в повторе редактор только для чтения): хеши совпадают.
# Уровни (модуль Challenge): бот эталонного решения проходит каждый уровень в настоящей игре; прохождение «Крепости» записывается и воспроизводится.
if(BUILD_TESTING)
    add_test(NAME Sandbox.RuneCell2.autoplay COMMAND RuneCell2 --autoplay --ticks 300 --autosave rune_cell2_test_autosave.fluxlog)
    set_tests_properties(Sandbox.RuneCell2.autoplay PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120 PASS_REGULAR_EXPRESSION "RuneCell2: OK" FAIL_REGULAR_EXPRESSION "ОШИБКА")
    add_test(NAME Sandbox.RuneCell2.record COMMAND RuneCell2 --autoplay --ticks 400 --record rune_cell2_test.rec)
    set_tests_properties(Sandbox.RuneCell2.record PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120 FIXTURES_SETUP rune_cell2_record PASS_REGULAR_EXPRESSION "recorded 400 ticks")
    add_test(NAME Sandbox.RuneCell2.replay COMMAND RuneCell2 --replay rune_cell2_test.rec)
    set_tests_properties(Sandbox.RuneCell2.replay PROPERTIES LABELS "gpu;sandbox" TIMEOUT 180 FIXTURES_REQUIRED rune_cell2_record
                         PASS_REGULAR_EXPRESSION "hashes MATCH the recording" FAIL_REGULAR_EXPRESSION "DIFFER FROM")
    foreach(level IN ITEMS walk mine mound)
        add_test(NAME Sandbox.RuneCell2.level.${level} COMMAND RuneCell2 --level ${level} --bot --ticks 300)
        set_tests_properties(Sandbox.RuneCell2.level.${level} PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120 PASS_REGULAR_EXPRESSION "Level ${level}: WON" FAIL_REGULAR_EXPRESSION "ОШИБКА")
    endforeach()
    add_test(NAME Sandbox.RuneCell2.level.fort COMMAND RuneCell2 --level fort --bot --ticks 800 --record rune_cell2_fort.rec)
    set_tests_properties(Sandbox.RuneCell2.level.fort PROPERTIES LABELS "gpu;sandbox" TIMEOUT 120 FIXTURES_SETUP rune_cell2_fort PASS_REGULAR_EXPRESSION "Level fort: WON")
    add_test(NAME Sandbox.RuneCell2.level.fort.replay COMMAND RuneCell2 --level fort --replay rune_cell2_fort.rec)
    set_tests_properties(Sandbox.RuneCell2.level.fort.replay PROPERTIES LABELS "gpu;sandbox" TIMEOUT 180 FIXTURES_REQUIRED rune_cell2_fort
                         PASS_REGULAR_EXPRESSION "Level fort: WON.*hashes MATCH the recording" FAIL_REGULAR_EXPRESSION "DIFFER FROM")
endif()

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
