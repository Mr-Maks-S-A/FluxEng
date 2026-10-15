# Тесты Wave и Mixer не требуют звуковой карты. Тест Device использует драйвер SDL «dummy» (звука нет, время идёт).
add_executable(AudioSystemTests
    test_main.cpp
    test_wave.cpp
    test_mixer.cpp
    test_device.cpp
)
target_link_libraries(AudioSystemTests PRIVATE engine::AudioSystem AudioSystemOptions doctest::doctest)
add_test(NAME AudioSystem.unit COMMAND AudioSystemTests)
set_tests_properties(AudioSystem.unit PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")
