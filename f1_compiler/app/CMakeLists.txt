
add_executable(keySwitch_test KeySwitchBackendTest/main.cpp)
add_executable(f1model_test F1Model/main.cpp)

target_link_libraries(keySwitch_test f1_lib)
target_link_libraries(f1model_test f1_lib)