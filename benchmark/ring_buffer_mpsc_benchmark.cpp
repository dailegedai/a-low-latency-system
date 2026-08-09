#include "../include/LockedRingBuffer.h"
#include "benchmark_util.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static void run_scenario(int producers, long per_worker)
{
    const long total = producers * per_worker;
    const std::string tag = std::to_string(producers) + "p";

    // A: 当前 ThreadPool 模型 —— std::queue + mutex
    {
        std::queue<int> q;
        std::mutex mtx;
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> ps;
        ps.reserve(producers);

        auto t0 = Clock::now();
        for (int t = 0; t < producers; ++t) {
            ps.emplace_back([&]() {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (long i = 0; i < per_worker; ++i) {
                    std::lock_guard<std::mutex> l(mtx);
                    q.push(1);
                }
            });
        }
        while (ready.load(std::memory_order_acquire) < producers) {
            std::this_thread::yield();
        }

        std::thread consumer([&]() {
            long got = 0;
            int x;
            while (got < total) {
                std::lock_guard<std::mutex> l(mtx);
                if (!q.empty()) {
                    x = q.front();
                    q.pop();
                    ++got;
                }
            }
            (void)x;
        });

        start.store(true, std::memory_order_release);
        consumer.join();
        for (auto& th : ps) {
            th.join();
        }
        auto t1 = Clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::cout << "  std::queue+mutex [" << tag << "]: " << total
                  << " items / " << ms << " ms\n";
    }

    // B: LockedRingBuffer（线程安全环）
    {
        LockedRingBuffer<int> q(total);
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> ps;
        ps.reserve(producers);

        auto t0 = Clock::now();
        for (int t = 0; t < producers; ++t) {
            ps.emplace_back([&]() {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (long i = 0; i < per_worker; ++i) {
                    q.push(1);
                }
            });
        }
        while (ready.load(std::memory_order_acquire) < producers) {
            std::this_thread::yield();
        }

        std::thread consumer([&]() {
            long got = 0;
            int x;
            while (got < total) {
                if (q.pop(x)) {
                    ++got;
                }
            }
        });

        start.store(true, std::memory_order_release);
        consumer.join();
        for (auto& th : ps) {
            th.join();
        }
        auto t1 = Clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::cout << "  LockedRingBuffer [" << tag << "]: " << total
                  << " items / " << ms << " ms\n";
    }
}

int main()
{
    for (int p : {1, 2, 4, 8}) {
        run_scenario(p, 100000);
    }
    return 0;
}
