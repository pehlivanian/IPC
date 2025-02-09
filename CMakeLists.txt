cmake_minimum_required(VERSION 3.11)

project(proto)

set(CMAKE_CXX_FLAGS "-mavx2 -Wall -Wextra -O3")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_executable(fifo_udp_comp fifo_udp_comp.cpp)
add_executable(fifo_unixDS_mq_open_comp fifo_unixDS_mq_open_comp.cpp)
add_executable(benchmarks benchmarks.cpp)

target_link_libraries(fifo_unixDS_mq_open_comp PRIVATE rt)
target_link_libraries(benchmarks PRIVATE rt)
