cmake_minimum_required(VERSION 3.10)

# Set the project name
project(layout_engine_test)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror -fprofile-instr-generate -fcoverage-mapping")
cmake_policy(SET CMP0135 NEW)

add_executable(layout_engine_test main.cpp)
