# ==============================================================================
# Общие тесты FluxEng: как модули работают ВМЕСТЕ.
#
# Юнит-тесты каждого модуля живут в Modules/<Name>/tests и проверяют модуль в одиночку
# (tools/check_modules.sh собирает их отдельно от движка). Здесь — стыки:
#
#   EventSystem + ECSSystem               ссылки на сущности в событиях, поколения, контракты модулей
#   JobSystem + MemorySystem + ECSSystem  параллельная система над пулом ECS: арены потоков, ChunkBuffers,
#     + EventSystem                       одинаковый результат при 0, 1 и N потоках
#   MemorySystem + EventSystem            память тика (DoubleArena) живёт ровно столько же, сколько событие
#   Net + SpellSim + Challenge +          партия «пилот и писарь» по плохой сети: миры совпадают побитно, правки заклинаний
#     Replay                              идут командами SetProgram, партию можно записать и воспроизвести
#   CardDuel (Sandbox) + JobSystem        партия ИИ против ИИ: инварианты правил, детерминизм при любом
#     + MemorySystem + EventSystem        числе потоков, журнал через шину
#   WindowSystem + RendererSystem   [gpu] скрытое окно, Renderer3D + Renderer2D + шрифт + Framebuffer + PNG,
#                                         Camera3D совпадает с растеризацией
#   Core + все модули               [gpu] цикл App: порядок хуков, ввод (inject) → шина, память тика, скриншот
#
# Запуск: ctest --test-dir build -L integration   (без GPU: -L integration -LE gpu)
# ==============================================================================
add_executable(IntegrationTests
    integration/test_main.cpp
    integration/test_events_ecs.cpp
    integration/test_jobs_memory_ecs.cpp
    integration/test_tick_memory.cpp
    integration/test_card_duel.cpp
    integration/test_net_spellsim.cpp
    integration/test_render_pipeline.cpp
    integration/test_core_app.cpp
)
target_link_libraries(IntegrationTests PRIVATE engine::Core engine::ECSSystem engine::Net engine::Challenge CardDuelRules doctest::doctest)
target_compile_features(IntegrationTests PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(IntegrationTests PRIVATE /W4 /utf-8)
else()
    target_compile_options(IntegrationTests PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# Каждый TEST_CASE — отдельный тест CTest. Имена начинаются с «cpu:» или «gpu:» — по ним ставятся метки
# «integration» и «integration-gpu» (одно значение: doctest не умеет передать список меток).
# Метки CTest — регулярные выражения: -L integration берёт обе, -LE gpu отбрасывает GPU-тесты.
if(DEFINED doctest_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
endif()
include(doctest OPTIONAL RESULT_VARIABLE doctest_script)
if(doctest_script)
    doctest_discover_tests(IntegrationTests TEST_SPEC "cpu:*" TEST_PREFIX "Integration." TEST_LIST integration_cpu
                           PROPERTIES LABELS integration)
    doctest_discover_tests(IntegrationTests TEST_SPEC "gpu:*" TEST_PREFIX "Integration." TEST_LIST integration_gpu
                           PROPERTIES LABELS integration-gpu RUN_SERIAL ON)
else()
    add_test(NAME Integration.all COMMAND IntegrationTests)
    set_tests_properties(Integration.all PROPERTIES LABELS integration-gpu)
endif()

# Документация модулей: у каждого есть описание, примеры (включённые в сборку и тесты), тесты и описание заголовков.
# Сами примеры собираются и запускаются как тесты: ctest -L example.
add_test(NAME Docs.modules COMMAND "${CMAKE_SOURCE_DIR}/tools/check_module_docs.sh")
set_tests_properties(Docs.modules PROPERTIES LABELS "docs")
