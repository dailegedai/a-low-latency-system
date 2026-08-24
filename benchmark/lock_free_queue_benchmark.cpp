#include "../include/LockFreeQueue.h"
#include "../include/LockedRingBuffer.h"
#include "benchmark_util.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// =====================================================================
// 统一 MPSC 压测：producers 个生产者各写 per_worker 个 int，单消费者读到 total。
// iteration() 执行一轮完整场景（含线程创建/join），sample() 做重复采样。
// =====================================================================

template <typename Q>
static void run_mpsc(const char* label, int producers, long per_worker)
{
    const long total = producers * per_worker;
    Q q(total);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    auto iteration = [&] {
        ready.store(0);
        start.store(false);
        std::vector<std::thread> ps;
        ps.reserve(producers);

        for (int t = 0; t < producers; ++t) {
            ps.emplace_back([&]() {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (long i = 0; i < per_worker; ++i) {
                    while (!q.push(1)) {
                        std::this_thread::yield();
                    }
                }
            });
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

        while (ready.load(std::memory_order_acquire) < producers) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);

        consumer.join();
        for (auto& th : ps) {
            th.join();
        }
    };

    sample(label, iteration, /*warmup=*/1, /*repeats=*/3);
}

// std::queue + mutex 走同一接口：单独实现（push/pop 语义不同）
static void run_std_queue_mutex(int producers, long per_worker)
{
    const long total = producers * per_worker;
    std::queue<int> q;
    std::mutex mtx;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    auto iteration = [&] {
        {
            std::lock_guard<std::mutex> l(mtx);
            while (!q.empty()) q.pop(); // 清空上一轮残留
        }
        ready.store(0);
        start.store(false);
        std::vector<std::thread> ps;
        ps.reserve(producers);

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

        while (ready.load(std::memory_order_acquire) < producers) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);

        consumer.join();
        for (auto& th : ps) {
            th.join();
        }
    };

    sample("std::queue+mutex", iteration, /*warmup=*/1, /*repeats=*/3);
}

int main()
{
    std::cout << "========== Lock-Free Queue Benchmark ==========\n\n";
    std::cout << "MPSC: N producers x 1 consumer, per_worker=100000\n";
    std::cout << "reporting median ms (lower is better)\n\n";

    for (int p : {1, 2, 4, 8}) {
        std::cout << "--- " << p << " producers ---\n";
        run_std_queue_mutex(p, 100000);
        run_mpsc<LockedRingBuffer<int>>("LockedRingBuffer", p, 100000);
        run_mpsc<LockFreeQueue<int>>("LockFreeQueue", p, 100000);
        std::cout << "\n";
    }

    return 0;
}
