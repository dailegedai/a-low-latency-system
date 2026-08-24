#include "../include/ThreadPool.h"
#include "benchmark_util.h"

#include <chrono>
#include <iostream>

using Clock = std::chrono::steady_clock;


// =====================================================================
// Part 1 — Micro: adjacent vs cache-line-separated counters
// =====================================================================

struct AdjacentCounter {
    std::atomic<long> v{0};
};

struct alignas(64) PaddedCounter {
    std::atomic<long> v{0};
};

constexpr int MAX_THREADS = 8;
static AdjacentCounter g_adjacent[MAX_THREADS];
static PaddedCounter g_padded[MAX_THREADS];

template <typename Counter>
static double run_micro(Counter* counters, const char* label, int num_threads, long iters_per_thread)
{
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (long i = 0; i < iters_per_thread; ++i) {
                counters[t].v.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < num_threads) {
        std::this_thread::yield();
    }

    double ms;
    {
        Benchmark b(std::string(label) + "threads=" + std::to_string(num_threads));
        start.store(true, std::memory_order_release);
        for (auto& th : threads) {
            th.join();
        }
        ms = static_cast<double>(b.elapsedMs());
    }
    return ms;
}

// =====================================================================
// Part 2 — Macro: ThreadPool throughput（测当前构建）
// =====================================================================

static void bench_threadpool(int thread_count, long tasks)
{
    ThreadPool pool(thread_count, 65536);
    std::atomic<long> done{0};

    sample(("threadpool threads=" + std::to_string(thread_count)).c_str(), [&] {
        done.store(0);
        for (long i = 0; i < tasks; ++i) {
            pool.submit([&done]() { done.fetch_add(1, std::memory_order_relaxed); });
        }
        while (done.load(std::memory_order_relaxed) < tasks) {
            std::this_thread::yield();
        }
    }, /*warmup=*/1, /*repeats=*/3);
}

int main()
{
     std::cout << "========== False Sharing Benchmark ==========\n";

    std::cout << "\n--- Part 1: Micro — adjacent vs padded ---\n";
    constexpr long ITERS = 100000000L;
    for (int t : {2, 4, 8}) {
        double adj = run_micro(g_adjacent, "adjacent", t, ITERS);
        double pad = run_micro(g_padded, "padded", t, ITERS);
        std::cout << "  speedup(" << t << " threads): " << (adj / pad) << "x\n";
    }

    std::cout << "\n--- Part 2: ThreadPool throughput (as built) ---\n";
    constexpr long TASKS = 500000;
    for (int t : {1, 2, 4, 8}) {
        bench_threadpool(t, TASKS);
    }

    std::cout << "\n========== Done ==========\n";
}