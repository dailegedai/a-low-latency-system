cmake_minimum_required(VERSION 3.10)

project(ll_engine)

enable_testing()

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
    src/WorkStealingThreadPool.cpp
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
    memory_pool_benchmark
    locked_ring_buffer_test
    cache_layout_probe
    ring_buffer_mpsc_benchmark
    latency_benchmark
    throughput_benchmark
    false_sharing_benchmark
    lock_free_queue_test
    lock_free_queue_benchmark
    work_stealing_threadpool_test
    work_stealing_threadpool_benchmark
    no_future_submit_test
    allocation_benchmark
    pipeline_demo
    task_trace_test
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
    false_sharing_benchmark
    benchmark/false_sharing_benchmark.cpp
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

add_executable(
    placement_new_test
    tests/placement_new_test.cpp
)
target_link_libraries(
    placement_new_test PRIVATE threadpool
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
    locked_ring_buffer_test
    tests/locked_ring_buffer_test.cpp
)

add_executable(
    lock_free_queue_test
    tests/lock_free_queue_test.cpp
)

add_executable(
    work_stealing_deque_test
    tests/work_stealing_deque_test.cpp
)

add_executable(
    work_stealing_queue_test
    tests/work_stealing_queue_test.cpp
)

add_executable(
    work_stealing_queue_benchmark
    benchmark/work_stealing_queue_benchmark.cpp
)

add_executable(
    light_epoch_test
    tests/light_epoch_test.cpp
)

add_executable(
    work_stealing_threadpool_test
    tests/work_stealing_threadpool_test.cpp
)

add_executable(
    work_stealing_threadpool_benchmark
    benchmark/work_stealing_threadpool_benchmark.cpp
)

add_executable(
    no_future_submit_test
    tests/no_future_submit_test.cpp
)

add_executable(
    allocation_benchmark
    benchmark/allocation_benchmark.cpp
)

add_executable(
    spsc_ring_buffer_test
    tests/spsc_ring_buffer_test.cpp
)

add_executable(
    task_trace_test
    tests/task_trace_test.cpp
)

add_executable(
    spsc_latency_benchmark
    benchmark/spsc_latency_benchmark.cpp
)

add_executable(
    pipeline_demo
    app/pipeline_demo.cpp
)

add_executable(
    cache_layout_probe
    tests/cache_layout_probe.cpp
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

add_executable(
    memory_pool_benchmark
    benchmark/memory_pool_benchmark.cpp
)

add_executable(
    ring_buffer_mpsc_benchmark
    benchmark/ring_buffer_mpsc_benchmark.cpp
)

add_executable(
    lock_free_queue_benchmark
    benchmark/lock_free_queue_benchmark.cpp
)

add_executable(
    nextpowerof2_benchmark
    benchmark/nextpowerof2_benchmark.cpp
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

# ==========================
# CTest Registration
# ==========================

# 每个测试可执行文件注册为独立 ctest 用例（含超时，防偶发挂起阻塞整轮）。
# 新增测试目标时同步加入此列表；benchmark/app 不注册。
set(LLENGINE_TEST_TARGETS

    bounded_queue_test
    reject_policy_test
    false_sharing_test
    metrics_test
    task_test
    shutdown_test
    memory_pool_test
    placement_new_test
    ring_buffer_test
    future_test
    packaged_task_test
    decltype_test
    locked_ring_buffer_test
    lock_free_queue_test
    cache_layout_probe
    work_stealing_deque_test
    work_stealing_queue_test
    light_epoch_test
    work_stealing_threadpool_test
    no_future_submit_test
    spsc_ring_buffer_test
    task_trace_test
)

foreach(test ${LLENGINE_TEST_TARGETS})

    add_test(NAME ${test} COMMAND ${test})
    set_tests_properties(${test} PROPERTIES TIMEOUT 180)

endforeach()