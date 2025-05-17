cmake_minimum_required(VERSION 3.11)

project(proto)

set(CMAKE_CXX_FLAGS "-mavx2 -Wall -Wextra -O3")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_VERBOSE_MAKEFILE ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories("/usr/include/dbus-1.0")
include_directories("/usr/lib/x86_64-linux-gnu/dbus-1.0/include")

set(AERON_LINK_LIB "aeron_client")
set(C_CLIENT_LINK_LIB "aeron_static")
# add_executable(fifo_udp_comp fifo_udp_comp.cpp)
# add_executable(fifo_unixDS_mq_open_comp fifo_unixDS_mq_open_comp.cpp)
add_executable(benchmarks benchmarks.cpp)

# target_link_libraries(fifo_unixDS_mq_open_comp PRIVATE rt)
target_link_libraries(benchmarks PRIVATE rt dbus-1 ${AERON_LINK_LIB})
