# Ядро рендера — без Terrain и ManaField: источники данных подменяются заглушками.
add_executable(WorldRenderTests test_main.cpp test_worldrender.cpp)
target_link_libraries(WorldRenderTests PRIVATE engine::WorldRender WorldRenderOptions doctest::doctest)
flux_doctest_discover(WorldRenderTests WorldRender)

# Готовые источники (ландшафт и туман маны): здесь Terrain и ManaField нужны.
add_executable(WorldRenderAdapterTests test_main.cpp test_adapters.cpp)
target_link_libraries(WorldRenderAdapterTests PRIVATE engine::WorldRenderTerrain WorldRenderOptions doctest::doctest)
flux_doctest_discover(WorldRenderAdapterTests WorldRenderAdapters)
