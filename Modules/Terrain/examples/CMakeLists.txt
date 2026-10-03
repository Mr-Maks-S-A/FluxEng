# Каждый пример — отдельная программа и одновременно smoke-тест CTest (проверяет сам себя и возвращает ненулевой код при ошибке).
set(TERRAIN_EXAMPLES
    01_dig_and_raycast
    02_custom_world
)
foreach(example IN LISTS TERRAIN_EXAMPLES)
    set(target TerrainExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::Terrain TerrainOptions)
    if(TERRAIN_BUILD_TESTS)
        add_test(NAME Terrain.example.${example} COMMAND ${target})
        set_tests_properties(Terrain.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
