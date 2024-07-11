cmake_minimum_required(VERSION 3.10)

project(shmem_connect)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
add_library(util util.cpp)

add_executable(shmem_server Server.cpp)
target_link_libraries(shmem_server util)


