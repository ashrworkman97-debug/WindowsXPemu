cmake_minimum_required(VERSION 3.20)
project(XP_Revival VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(xprevival_core
    src/cpu.cpp
    src/ata_pio.cpp
    src/pc.cpp
)
target_include_directories(xprevival_core PUBLIC include)

if(MSVC)
    target_compile_options(xprevival_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(xprevival_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(xprevival src/main.cpp)
target_link_libraries(xprevival PRIVATE xprevival_core)

enable_testing()
add_executable(cpu_tests tests/cpu_tests.cpp)
target_link_libraries(cpu_tests PRIVATE xprevival_core)
add_test(NAME cpu_tests COMMAND cpu_tests)

add_executable(pc_tests tests/pc_tests.cpp)
target_link_libraries(pc_tests PRIVATE xprevival_core)
add_test(NAME pc_tests COMMAND pc_tests)
