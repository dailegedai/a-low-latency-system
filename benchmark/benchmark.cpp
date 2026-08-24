#include "../include/ThreadPool.h"
#include "benchmark_util.h"

#include <atomic>
#include <iostream>

int main() {

    constexpr int TASK_NUM = 100000;

    ThreadPool pool(4, 100);

    std::atomic<int> counter{0};

    sample("end-to-end 100k tasks", [&] {
        counter.store(0);
        for (int i = 0; i < TASK_NUM; ++i) {
            pool.submit([&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        while (counter.load(std::memory_order_relaxed) != TASK_NUM) {
            std::this_thread::yield();
        }
    }, 1, 5);
}