cmake_minimum_required(VERSION 3.10)

project(ll_engine)

# ==========================
# C++ Standard
# ==========================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==========================
# Build Type
# ==========================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ==========================
# Compile Options
# ==========================
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

# ==========================
# Threads
# ==========================
find_package(Threads REQUIRED)

# ==========================
# Include
# ==========================
include_directories(include)

# ==========================
# ThreadPool Library
# ==========================

set(THREADPOOL_SRC
    src/MemoryPool.cpp
    src/ThreadPool.cpp
    src/Task.cpp
    src/Worker.cpp
)

add_library(
    threadpool
    ${THREADPOOL_SRC}
)

target_link_libraries(
    threadpool
    PUBLIC Threads::Threads
)

# ==========================
# Executables
# ==========================

set(THREADPOOL_TARGETS

    main
    benchmark
    bounded_queue_test
    reject_policy_test
    false_sharing_test
    submit_benchmark
    metrics_test
    task_test
    shutdown_test
    memory_pool_test
    latency_benchmark
    throughput_benchmark
)

add_executable(
    main
    app/main.cpp
)

add_executable(
    benchmark
    benchmark/benchmark.cpp
)

add_executable(
    bounded_queue_test
    tests/bounded_queue_test.cpp
)

add_executable(
    reject_policy_test
    tests/reject_policy_test.cpp
)

add_executable(
    false_sharing_test
    tests/false_sharing_test.cpp
)

add_executable(
    submit_benchmark
    benchmark/submit_benchmark.cpp
)

add_executable(
    metrics_test
    tests/metrics_test.cpp
)

add_executable(
    task_test
    tests/task_test.cpp
)

add_executable(
    shutdown_test
    tests/shutdown_test.cpp
)

add_executable(
    ring_buffer_test
    tests/ring_buffer_test.cpp
)

add_executable(
    memory_pool_test
    tests/memory_pool_test.cpp
)

# ==========================
# Standalone Tests
# ==========================

add_executable(
    future_test
    tests/future_test.cpp
)

add_executable(
    packaged_task_test
    tests/packaged_task_test.cpp
)

add_executable(
    decltype_test
    tests/decltype_test.cpp
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
    queue_benchmark
    benchmark/queue_benchmark.cpp
)

add_executable(
    latency_benchmark
    benchmark/latency_benchmark.cpp
)


add_executable(
    throughput_benchmark
    benchmark/throughput_benchmark.cpp
)

# ==========================
# Link ThreadPool
# ==========================

foreach(target ${THREADPOOL_TARGETS})

    target_link_libraries(
        ${target}
        PRIVATE threadpool
    )

endforeach()