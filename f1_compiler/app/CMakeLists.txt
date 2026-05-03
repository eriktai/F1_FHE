
add_executable(keySwitch_test KeySwitchBackendTest/main.cpp)
add_executable(f1model_test F1Model/main.cpp)
add_executable(loadPort_test LoadPortProfiling/main.cpp)
add_executable(figlut_test FIGLUT/main.cpp)

target_link_libraries(keySwitch_test f1_lib fmt)
target_link_libraries(f1model_test f1_lib fmt)
target_link_libraries(loadPort_test f1_lib fmt)
target_link_libraries(figlut_test f1_lib fmt)