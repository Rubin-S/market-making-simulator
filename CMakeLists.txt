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

add_executable(test_queue sim/tests/test_queue.cpp)
target_link_libraries(test_queue PRIVATE mm)
add_test(NAME queue COMMAND test_queue)

add_executable(test_fill sim/tests/test_fill.cpp)
target_link_libraries(test_fill PRIVATE mm)
add_test(NAME fill COMMAND test_fill)

add_executable(test_markout sim/tests/test_markout.cpp)
target_link_libraries(test_markout PRIVATE mm)
add_test(NAME markout COMMAND test_markout)

add_executable(test_glft sim/tests/test_glft.cpp)
target_link_libraries(test_glft PRIVATE mm)
add_test(NAME glft COMMAND test_glft)


# The simulator itself
add_executable(mm_sim sim/src/mm_sim.cpp)
target_link_libraries(mm_sim PRIVATE mm)

# Compat mode: hftbacktest's machinery + our queue model and ledger (checked by oracle/run_compat.py)
add_executable(mm_compat sim/src/mm_compat.cpp)
target_link_libraries(mm_compat PRIVATE mm)

# Top-of-book dump on the mm_sim clock (read by research/calibrate.py)
add_executable(mm_bbo sim/src/mm_bbo.cpp)
target_link_libraries(mm_bbo PRIVATE mm)

