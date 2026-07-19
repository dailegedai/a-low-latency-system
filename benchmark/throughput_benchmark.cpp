#include "../include/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>
#include <future>

using Clock = std::chrono::steady_clock;

struct Result {
    std::string name;
    int64_t elapsed_us;
    int64_t tasks;
    double throughput; // tasks/sec
};

template <typename Fn>
static Result measure(const char *name, int task_count, Fn &&setup)
{
    auto start = Clock::now();
    setup();
    auto end = Clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    return {name, us, task_count, task_count * 1000000.0 / us};
}

static double compute_pi(int terms)
{
    double sum = 0.0;
    for (int k = 0; k < terms; k++) {
        sum += (k % 2 == 0 ? 1.0 : -1.0) / (2 * k + 1);
    }
    return sum * 4.0;
}

int main()
{
    constexpr int N = 500000;

    std::cout << "=== Throughput Benchmark Suite ===\n\n";
    std::cout << "each test submits " << N << " tasks unless noted\n\n";

    std::vector<Result> results;

    // 1. Baseline throughput vs thread count
    {
        std::cout << "--- throughput vs thread count (empty tasks) ---\n";
        for (auto threads : {1, 2, 4, 8}) {
            std::atomic<int64_t> done{0};
            ThreadPool pool(threads, 65536);
            auto r = measure(("threads=" + std::to_string(threads)).c_str(), N, [&] {
                for (int i = 0; i < N; i++) {
                    pool.submit([&] { done.fetch_add(1, std::memory_order_relaxed); });
                }
                while (done.load(std::memory_order_relaxed) != N)
                    std::this_thread::yield();
            });
            pool.shutdown();
            results.push_back(r);
            std::cout << "  " << r.name << ": " << r.throughput << " tasks/sec\n";
        }
        std::cout << "\n";
    }

    // 2. Throughput vs queue size
    {
        std::cout << "--- throughput vs queue size (4 threads, empty tasks) ---\n";
        for (auto qsize : {4, 16, 64, 256, 1024, 65536}) {
            std::atomic<int64_t> done{0};
            ThreadPool pool(4, qsize);
            auto r = measure(("queue=" + std::to_string(qsize)).c_str(), N, [&] {
                for (int i = 0; i < N; i++) {
                    pool.submit([&] { done.fetch_add(1, std::memory_order_relaxed); });
                }
                while (done.load(std::memory_order_relaxed) != N)
                    std::this_thread::yield();
            });
            pool.shutdown();
            results.push_back(r);
            std::cout << "  " << r.name << ": " << r.throughput << " tasks/sec\n";
        }
        std::cout << "\n";
    }

    // 3. Throughput with varying work weight
    {
        std::cout << "--- throughput vs work weight (4 threads, queue=65536) ---\n";
        struct Work {
            const char *name;
            int terms;
        };
        for (auto w : {Work{"empty", 0}, Work{"pi(10)", 10}, Work{"pi(100)", 100}, Work{"pi(1000)", 1000}}) {
            int count = (w.terms == 0) ? N : N / 10;
            std::atomic<int64_t> done{0};
            ThreadPool pool(4, 65536);
            auto r = measure(w.name, count, [&] {
                for (int i = 0; i < count; i++) {
                    pool.submit([&] { if (w.terms) compute_pi(w.terms); done.fetch_add(1, std::memory_order_relaxed); });
                }
                while (done.load(std::memory_order_relaxed) != count)
                    std::this_thread::yield();
            });
            pool.shutdown();
            results.push_back(r);
            std::cout << "  " << r.name << ": " << r.throughput << " tasks/sec\n";
        }
        std::cout << "\n";
    }

    // 4. Throughput with future.get() overhead
    {
        std::cout << "--- throughput with future.get() (4 threads, queue=65536) ---\n";
        {
            ThreadPool pool(4, 65536);
            auto r = measure("void future.get()", N, [&] {
                std::vector<std::future<void>> futs;
                futs.reserve(N);
                for (int i = 0; i < N; i++)
                    futs.push_back(pool.submit([] {}));
                for (auto &f : futs)
                    f.get();
            });
            pool.shutdown();
            results.push_back(r);
            std::cout << "  " << r.name << ": " << r.throughput << " tasks/sec\n";
        }
        {
            ThreadPool pool(4, 65536);
            auto r = measure("int future.get()", N, [&] {
                std::vector<std::future<int>> futs;
                futs.reserve(N);
                for (int i = 0; i < N; i++)
                    futs.push_back(pool.submit([] { return 42; }));
                volatile int sum = 0;
                for (auto &f : futs)
                    sum += f.get();
            });
            pool.shutdown();
            results.push_back(r);
            std::cout << "  " << r.name << ": " << r.throughput << " tasks/sec\n";
        }
        std::cout << "\n";
    }

    // 5. Throughput under queue full (BLOCK policy)
    {
        std::cout << "--- throughput under queue full (BLOCK, 2 threads) ---\n";
        for (auto qsize : {4, 1024}) {
            std::atomic<int64_t> done{0};
            ThreadPool pool(2, qsize, RejectPolicy::BLOCK);
            int count = N / 10;
            auto r = measure(("BLOCK queue=" + std::to_string(qsize)).c_str(), count, [&] {
                for (int i = 0; i < count; i++) {
                    pool.submit([&] { compute_pi(500); done.fetch_add(1, std::memory_order_relaxed); });
                }
                while (done.load(std::memory_order_relaxed) != count)
                    std::this_thread::yield();
            });
            pool.shutdown();
            results.push_back(r);
            std::cout << "  " << r.name << ": " << r.throughput << " tasks/sec\n";
        }
        std::cout << "\n";
    }

    // 6. Sustained throughput (larger batch)
    {
        std::cout << "--- sustained throughput (4 threads, queue=65536, empty tasks) ---\n";
        constexpr int BIG = 2000000;
        std::atomic<int64_t> done{0};
        ThreadPool pool(4, 65536);
        auto r = measure("2M tasks", BIG, [&] {
            for (int i = 0; i < BIG; i++) {
                pool.submit([&] { done.fetch_add(1, std::memory_order_relaxed); });
            }
            while (done.load(std::memory_order_relaxed) != BIG)
                std::this_thread::yield();
        });
        pool.shutdown();
        results.push_back(r);
        std::cout << "  " << r.name << ": " << r.throughput << " tasks/sec\n\n";
    }

    // Summary
    std::cout << "=== Summary (tasks/sec) ===\n";
    for (auto &r : results)
        std::cout << "  " << r.name << ": " << r.throughput << "\n";

    std::cout << "\n=== Done ===\n";
    return 0;
}
