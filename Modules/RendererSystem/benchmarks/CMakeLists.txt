add_executable(RendererSystemBenchmarks benchmark_main.cpp)

target_link_libraries(RendererSystemBenchmarks PRIVATE
    engine::RendererSystem
    RendererSystemOptions
    benchmark::benchmark
    glfw
)
renderersystem_system_includes(RendererSystemBenchmarks glfw benchmark::benchmark)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(MSVC)
        target_compile_options(RendererSystemBenchmarks PRIVATE /O2 /Ob2)
    else()
        target_compile_options(RendererSystemBenchmarks PRIVATE -O3)
    endif()
endif()
