cmake_minimum_required(VERSION 3.20)
project(market_making_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic)

# Header-only simulator library: our headers + the pinned engine's headers
add_library(mm INTERFACE)
target_include_directories(mm INTERFACE sim/include engine/include)

enable_testing()
add_executable(test_core sim/tests/test_core.cpp)
target_link_libraries(test_core PRIVATE mm)
add_test(NAME core COMMAND test_core)
