# Каждый пример — отдельная программа и smoke-тест CTest (проверяет сам себя, ненулевой код при ошибке).
# 01 — без окна и GPU; 02 рисует кадр во внеэкранную цель (скрытое окно) и сохраняет PNG.
add_executable(WorldRenderExample_01_data_sources 01_data_sources.cpp)
target_link_libraries(WorldRenderExample_01_data_sources PRIVATE engine::WorldRender WorldRenderOptions)

add_executable(WorldRenderExample_02_render_terrain_png 02_render_terrain_png.cpp)
target_link_libraries(WorldRenderExample_02_render_terrain_png PRIVATE engine::WorldRenderTerrain engine::WindowSystem glfw WorldRenderOptions)

if(WORLDRENDER_BUILD_TESTS)
    add_test(NAME WorldRender.example.01_data_sources COMMAND WorldRenderExample_01_data_sources)
    set_tests_properties(WorldRender.example.01_data_sources PROPERTIES LABELS "example")
    add_test(NAME WorldRender.example.02_render_terrain_png COMMAND WorldRenderExample_02_render_terrain_png "${CMAKE_CURRENT_BINARY_DIR}/worldrender_scene.png")
    set_tests_properties(WorldRender.example.02_render_terrain_png PROPERTIES LABELS "example;gpu" TIMEOUT 60)
endif()
