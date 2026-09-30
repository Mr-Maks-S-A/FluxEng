# Примеры. 03 работает без GPU; 01 и 02 используют окно движка (engine::WindowSystem)
# и собираются, только если модуль WindowSystem доступен (его подключает flux_module).

function(renderersystem_add_example name)
    set(target RendererSystemExample_${name})
    add_executable(${target} ${name}.cpp)
    target_link_libraries(${target} PRIVATE engine::RendererSystem RendererSystemOptions ${ARGN})
endfunction()

renderersystem_add_example(03_data_oriented_frame)
if(RENDERERSYSTEM_BUILD_TESTS)
    add_test(NAME RendererSystem.example.03_data_oriented_frame COMMAND RendererSystemExample_03_data_oriented_frame)
endif()

if(TARGET engine::WindowSystem)
    renderersystem_add_example(01_window_sprites engine::WindowSystem glfw)
    renderersystem_add_example(02_offscreen_png engine::WindowSystem glfw stb)

    if(RENDERERSYSTEM_BUILD_TESTS)
        # Smoke-тесты с GPU: окно на 120 кадров и рендер в PNG.
        add_test(NAME RendererSystem.example.01_window_sprites
                 COMMAND RendererSystemExample_01_window_sprites --frames 120)
        add_test(NAME RendererSystem.example.02_offscreen_png
                 COMMAND RendererSystemExample_02_offscreen_png "${CMAKE_CURRENT_BINARY_DIR}/renderer_offscreen.png")
        set_tests_properties(RendererSystem.example.01_window_sprites RendererSystem.example.02_offscreen_png
                             PROPERTIES LABELS "gpu")
        if(RENDERERSYSTEM_SANITIZE)
            # Window::~Window вызывает glfwTerminate, драйвер GPU выгружается раньше отчёта
            # LeakSanitizer, и его внутренние аллокации (и fontconfig) выглядят как утечки.
            # Это не код модуля; утечки RendererSystem проверяют юнит-тесты.
            set_tests_properties(RendererSystem.example.01_window_sprites RendererSystem.example.02_offscreen_png
                                 PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
        endif()
    endif()
else()
    message(STATUS "RendererSystem: engine::WindowSystem не подключён — примеры 01 и 02 не собираются")
endif()
