# 01 открывает окно (нужен дисплей, метка gpu); 02 работает без окна.
foreach(example IN ITEMS 01_window_input 02_input_replay)
    set(target WindowSystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::WindowSystem WindowSystemOptions)
endforeach()

if(WINDOWSYSTEM_BUILD_TESTS)
    add_test(NAME WindowSystem.example.01_window_input COMMAND WindowSystemExample_01_window_input --frames 60)
    set_tests_properties(WindowSystem.example.01_window_input PROPERTIES LABELS "example;gpu")
    add_test(NAME WindowSystem.example.02_input_replay COMMAND WindowSystemExample_02_input_replay)
    set_tests_properties(WindowSystem.example.02_input_replay PROPERTIES LABELS "example")
endif()
