# ==============================================================================
# Target: App (Sandbox)
# ==============================================================================

add_executable(App
    src/main.cpp
)

target_link_libraries(App PRIVATE
    engine::EventSystem
    engine::ECSSystem

    engine::WindowSystem
    engine::RendererSystem
    glad
)

target_compile_features(App PRIVATE cxx_std_20)