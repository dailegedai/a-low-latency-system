cmake_minimum_required(VERSION 3.10)

project(ll_engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Debug / Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 编译警告
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

# pthread
find_package(Threads REQUIRED)

# include 路径
include_directories(include)

# ThreadPool library
add_library(
    threadpool
    src/ThreadPool.cpp
)

target_link_libraries(
    threadpool
    Threads::Threads
)

# main executable
add_executable(
    main
    app/main.cpp
)

target_link_libraries(
    main
    threadpool
)

# benchmark executable
add_executable(
    benchmark
    benchmark/benchmark.cpp
)

target_link_libraries(
    benchmark
    threadpool
)