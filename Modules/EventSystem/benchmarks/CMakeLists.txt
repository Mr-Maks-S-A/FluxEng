add_executable(EventSystemBenchmarks benchmark_main.cpp)

target_link_libraries(EventSystemBenchmarks PRIVATE
    engine::EventSystem
    EventSystemOptions
    benchmark::benchmark
)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(MSVC)
        target_compile_options(EventSystemBenchmarks PRIVATE /O2 /Ob2)
    else()
        target_compile_options(EventSystemBenchmarks PRIVATE -O3)
    endif()
endif()
