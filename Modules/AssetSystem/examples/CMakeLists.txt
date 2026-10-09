# Каждый пример — отдельная программа и одновременно smoke-тест CTest.
set(ASSETSYSTEM_EXAMPLES
    01_cook_and_load
    02_async_loading
    03_mods_and_untrusted_data
)
foreach(example IN LISTS ASSETSYSTEM_EXAMPLES)
    set(target AssetSystemExample_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE engine::AssetSystem AssetSystemOptions)
    if(ASSETSYSTEM_BUILD_TESTS)
        add_test(NAME AssetSystem.example.${example} COMMAND ${target})
        set_tests_properties(AssetSystem.example.${example} PROPERTIES LABELS "example")
    endif()
endforeach()
