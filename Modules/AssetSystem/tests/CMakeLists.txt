add_executable(AssetSystemTests
    test_main.cpp
    test_paths_vfs.cpp
    test_pack.cpp
    test_loaders.cpp
    test_manager.cpp
)
target_link_libraries(AssetSystemTests PRIVATE engine::AssetSystem AssetSystemOptions doctest::doctest stb)
flux_doctest_discover(AssetSystemTests AssetSystem)
