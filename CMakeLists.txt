cmake_minimum_required(VERSION 3.20)
project(trigger_ac7vr LANGUAGES CXX)

add_library(xinput1_3 SHARED
    src/graphics_bridge.cpp
    src/xinput_proxy.cpp
    src/xinput1_3.def
)

target_compile_features(xinput1_3 PRIVATE cxx_std_20)
target_compile_definitions(xinput1_3 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(xinput1_3 PRIVATE third_party/openxr/include)
target_link_libraries(xinput1_3 PRIVATE user32 d3d11 dxgi advapi32)
set_target_properties(xinput1_3 PROPERTIES
    OUTPUT_NAME "xinput1_3"
    PREFIX ""
)

include(CTest)
if(BUILD_TESTING)
    add_executable(graphics_bridge_math_test
        tests/graphics_bridge_math_test.cpp
        src/graphics_bridge.cpp
    )
    target_compile_features(graphics_bridge_math_test PRIVATE cxx_std_20)
    target_compile_definitions(graphics_bridge_math_test PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX TRIGGER_AC7VR_ABI_TEST)
    target_include_directories(graphics_bridge_math_test PRIVATE third_party/openxr/include)
    target_link_libraries(graphics_bridge_math_test PRIVATE d3d11 dxgi advapi32)
    add_test(NAME graphics_bridge_math COMMAND graphics_bridge_math_test)

    add_executable(fake_interface_abi_test
        tests/fake_interface_abi_test.cpp
    )
    target_compile_features(fake_interface_abi_test PRIVATE cxx_std_20)
    target_compile_definitions(fake_interface_abi_test PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX TRIGGER_AC7VR_ABI_TEST)
    target_include_directories(fake_interface_abi_test PRIVATE third_party/openxr/include)
    target_link_libraries(fake_interface_abi_test PRIVATE user32 d3d11 dxgi advapi32)
    add_test(NAME fake_interface_abi COMMAND fake_interface_abi_test)
endif()
