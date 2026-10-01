cmake_minimum_required(VERSION 3.20)
project(trigger_ac7vr LANGUAGES CXX)

add_library(xinput1_3 SHARED
    src/xinput_proxy.cpp
    src/xinput1_3.def
)

target_compile_features(xinput1_3 PRIVATE cxx_std_20)
target_compile_definitions(xinput1_3 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xinput1_3 PRIVATE user32)
set_target_properties(xinput1_3 PROPERTIES
    OUTPUT_NAME "xinput1_3"
    PREFIX ""
)
