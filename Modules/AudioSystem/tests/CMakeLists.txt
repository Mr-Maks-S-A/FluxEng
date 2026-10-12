add_executable(AudioSystemTests
    test_main.cpp
    test_clip.cpp
    test_dsp.cpp
    test_engine.cpp
    test_spatial.cpp
    test_events.cpp
)
target_link_libraries(AudioSystemTests PRIVATE engine::AudioSystem AudioSystemOptions doctest::doctest)
flux_doctest_discover(AudioSystemTests AudioSystem)
