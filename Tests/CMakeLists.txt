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
#   CardDuel (Sandbox) + JobSystem        партия ИИ против ИИ: инварианты правил, детерминизм при любом
#     + MemorySystem + EventSystem        числе потоков, журнал через шину
#   WindowSystem + RendererSystem   [gpu] скрытое окно, Renderer3D + Renderer2D + шрифт + Framebuffer + PNG,
#                                         Camera3D совпадает с растеризацией
#   Core + все модули               [gpu] цикл App: порядок хуков, ввод (inject) → шина, память тика, скриншот
#   RuntimeSystem + ECS + Jobs +          «выделенный сервер» без окна: порядок init по зависимостям, один результат
#     EventSystem + MemorySystem          при 0, 1 и 4 потоках, shutdown при исключении
#   InputSystem + NetSystem               ввод → действия → команда → lockstep: разные устройства и привязки, плохая сеть, реплей
#   Core + RuntimeSystem            [gpu] модули внутри клиента: configure → init → setup, обратное завершение, откат
#
# Запуск: ctest --test-dir build -L integration   (без GPU: -L integration -LE gpu)
# ==============================================================================
add_executable(IntegrationTests
    integration/test_main.cpp
    integration/test_events_ecs.cpp
    integration/test_jobs_memory_ecs.cpp
    integration/test_tick_memory.cpp
    integration/test_card_duel.cpp
    integration/test_render_pipeline.cpp
    integration/test_core_app.cpp
    integration/test_runtime_headless.cpp
    integration/test_app_modules.cpp
    integration/test_input_lockstep.cpp
)
target_link_libraries(IntegrationTests PRIVATE engine::Core engine::InputSystem engine::NetSystem engine::RuntimeSystem engine::ECSSystem glfw glad CardDuelRules doctest::doctest)
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
