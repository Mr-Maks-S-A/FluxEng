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
    test_gl.cpp
)

target_link_libraries(RendererSystemTests PRIVATE
    engine::RendererSystem
    RendererSystemOptions
    doctest::doctest
    glfw
    stb
)
renderersystem_system_includes(RendererSystemTests glfw stb doctest::doctest)

if(DEFINED doctest_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
endif()
include(doctest OPTIONAL RESULT_VARIABLE DOCTEST_INCLUDED)
if(DOCTEST_INCLUDED)
    doctest_discover_tests(RendererSystemTests TEST_PREFIX "RendererSystem.")
else()
    add_test(NAME RendererSystem.unit COMMAND RendererSystemTests)
endif()
