#include "../include/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <iostream>

int main() {

    constexpr int TASK_NUM = 100000;

    ThreadPool pool(4, 100);

    std::atomic<int> counter{0};

    auto start =
        std::chrono::steady_clock::now();

    for (int i = 0; i < TASK_NUM; ++i) {

        pool.submit([&counter]() {
            counter.fetch_add(
                1,
                std::memory_order_relaxed
            );
        });
    }

    while (counter.load(
        std::memory_order_relaxed
    ) != TASK_NUM)
    {
        std::this_thread::yield();
    }

    auto end =
        std::chrono::steady_clock::now();

    auto us =
        std::chrono::duration_cast<
            std::chrono::microseconds>(
                end - start);

    std::cout
        << "tasks: "
        << TASK_NUM
        << "\n";

    std::cout
        << "time(us): "
        << us.count()
        << "\n";

    std::cout
        << "throughput(task/s): "
        << TASK_NUM * 1000000.0
           / us.count()
        << "\n";
}