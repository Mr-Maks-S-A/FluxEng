# ==============================================================================
# Юнит-тесты (doctest)
#
# CPU-тесты работают везде. GPU-тесты (test_gl.cpp) создают скрытое окно GLFW;
# если OpenGL 3.3 недоступен, они пропускаются с сообщением, а не падают.
# ==============================================================================
add_executable(RendererSystemTests
    test_main.cpp
    test_core.cpp
    test_camera.cpp
    test_sprite_batch.cpp
    test_animation.cpp
    test_3d.cpp
    test_text.cpp
    test_render.cpp
)

target_link_libraries(RendererSystemTests PRIVATE
    engine::RendererSystem
    RendererSystemOptions
    doctest::doctest
    glfw
    stb
)

flux_doctest_discover(RendererSystemTests RendererSystem)
