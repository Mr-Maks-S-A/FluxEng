add_executable(EventSystemBenchmarks benchmark_main.cpp)

# Подключаем нашу библиотеку событий и google-benchmark
target_link_libraries(EventSystemBenchmarks PRIVATE
    engine::EventSystem
    benchmark::benchmark
)

# Для бенчмарков требуется C++23, как и для основного модуля
target_compile_features(EventSystemBenchmarks PRIVATE cxx_std_23)

# Автоматическая установка флагов оптимизации для режима Release
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(MSVC)
        target_compile_options(EventSystemBenchmarks PRIVATE /O2 /Ob2)
    else()
        target_compile_options(EventSystemBenchmarks PRIVATE -O3 -flto)
    endif()
endif()




