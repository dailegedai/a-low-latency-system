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

# =========================
# future test
# =========================
add_executable(future_test
    tests/future_test.cpp
)

# =========================
# packaged_task test
# =========================
add_executable(packaged_task_test
    tests/packaged_task_test.cpp
)

# =========================
# decltype template invoke_result test
# =========================
add_executable(decltype_test
    tests/decltype_test.cpp
)

add_executable(
    bounded_queue_test
    tests/bounded_queue_test.cpp
)

target_link_libraries(
    bounded_queue_test
    threadpool
)

add_executable(
    reject_policy_test
    tests/reject_policy_test.cpp
)

target_link_libraries(
    reject_policy_test
    threadpool
)

add_executable(
    false_sharing_test
    tests/false_sharing_test.cpp
)

target_link_libraries(
    false_sharing_test
    threadpool
)

add_executable(
    function_benchmark
    benchmark/function_benchmark.cpp
)

add_executable(
    lambda_benchmark
    benchmark/lambda_benchmark.cpp
)

add_executable(
    submit_benchmark
    benchmark/submit_benchmark.cpp
)

target_link_libraries(
    submit_benchmark
    threadpool
)


