add_executable(ECSSystemBenchmarks benchmark_main.cpp)

target_link_libraries(ECSSystemBenchmarks PRIVATE
    engine::ECSSystem
    benchmark::benchmark
)

target_compile_features(ECSSystemBenchmarks PRIVATE cxx_std_23)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(MSVC)
        target_compile_options(ECSSystemBenchmarks PRIVATE /O2 /Ob2)
    else()
        target_compile_options(ECSSystemBenchmarks PRIVATE -O3 -flto)
    endif()
endif()