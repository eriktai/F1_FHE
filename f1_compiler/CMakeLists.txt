cmake_minimum_required(VERSION 4.3.0)
project(f1_compiler)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(src)
include_directories(include)

add_subdirectory(src)
add_subdirectory(app)

