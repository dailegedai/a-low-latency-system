#include "benchmark_util.h"
#include "../include/MemoryPool.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

struct LatencyStats
{
    double min{0};
    double avg{0};
    double max{0};
    double p50{0};
    double p99{0};
};

static LatencyStats compute_stats(std::vector<double> &samples)
{
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    LatencyStats s;
    s.min = samples.front();
    s.max = samples.back();
    s.avg = std::accumulate(samples.begin(), samples.end(), 0.0) / n;
    s.p50 = samples[n / 2];
    s.p99 = samples[static_cast<size_t>(n * 0.99)];
    return s;
}

static void print_stats(const char *label, const LatencyStats &s)
{
    std::cout << "  " << label << ":\n";
    std::cout << "    min: " << s.min << " ns\n";
    std::cout << "    avg: " << s.avg << " ns\n";
    std::cout << "    max: " << s.max << " ns\n";
    std::cout << "    p50: " << s.p50 << " ns\n";
    std::cout << "    p99: " << s.p99 << " ns\n";
}

static void test_latency()
{
    std::cout << "\n---------- Latency Comparison (single-thread) ----------\n";

    constexpr int WARMUP = 100000;
    constexpr int SAMPLES = 1000000;

    std::vector<double> pool_ns;
    std::vector<double> raw_ns;
    pool_ns.reserve(SAMPLES);
    raw_ns.reserve(SAMPLES);

    // warmup
    {
        MemoryPool m(1024);
        for (int i = 0; i < WARMUP; ++i)
        {
            Task *t = m.acquire();
            m.release(t);
        }
    }
    {
        for (int i = 0; i < WARMUP; ++i)
        {
            Task *t = new Task([]() {});
            delete t;
        }
    }

    // pool latency（acquire + release 均计入，与 raw new+delete 对照）
    {
        MemoryPool m(1024);
        for (int i = 0; i < SAMPLES; ++i)
        {
            auto start = Clock::now();
            Task *t = m.acquire();
            m.release(t);
            auto end = Clock::now();
            pool_ns.push_back(
                std::chrono::duration_cast<ns>(end - start).count());
        }
    }

    // raw new/delete latency（new + delete 均计入计时区间，与 Pool acquire+release 对照）
    {
        for (int i = 0; i < SAMPLES; ++i)
        {
            auto start = Clock::now();
            Task *t = new Task([]() {});
            delete t;
            auto end = Clock::now();
            raw_ns.push_back(
                std::chrono::duration_cast<ns>(end - start).count());
        }
    }

    LatencyStats ps = compute_stats(pool_ns);
    LatencyStats rs = compute_stats(raw_ns);

    print_stats("Pool acquire", ps);
    print_stats("Raw new/delete", rs);

    std::cout << "  Speed-up (avg): " << (rs.avg / ps.avg) << "x\n";
    std::cout << "  Speed-up (p99): " << (rs.p99 / ps.p99) << "x\n";
}

/* =====================================================================
 * Throughput
 * ===================================================================== */

static void test_throughput()
{
    std::cout << "\n---------- Throughput (single-thread) ----------\n";

    constexpr int OPS = 1000000;

    Stats pool_stats = sample_with_setup(
        "Pool acquire+release",
        [] { return std::make_unique<MemoryPool>(1024); },
        [](std::unique_ptr<MemoryPool>& pool) {
            for (int i = 0; i < OPS; ++i) {
                Task* task = pool->acquire();
                if (!task) {
                    throw std::runtime_error("pool unexpectedly exhausted");
                }
                pool->release(task);
            }
        },
        1,
        3);

    Stats raw_stats = sample(
        "Raw new/delete",
        [&] {
            for (int i = 0; i < OPS; ++i) {
                Task* task = new Task([] {});
                delete task;
            }
        },
        1,
        3);

    std::cout << "  Pool throughput: "
              << static_cast<long>(OPS * 1000.0 / pool_stats.median_ms)
              << " ops/sec\n";
    std::cout << "  Raw  throughput: "
              << static_cast<long>(OPS * 1000.0 / raw_stats.median_ms)
              << " ops/sec\n";
}

/* =====================================================================
 * Contended pool (shared + mutex)
 * ===================================================================== */

static void test_contended()
{
    std::cout << "\n---------- Contended Pool (shared + mutex) ----------\n";

    for (int thread_count : {2, 4, 8})
    {
        constexpr int OPS_PER_THREAD = 500000;

        MemoryPool shared_pool(thread_count * 64);
        std::atomic<int> ready{0};
        std::atomic<bool> start_flag{false};

        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        auto start = Clock::now();

        for (int t = 0; t < thread_count; ++t)
        {
            threads.emplace_back([&]()
                                 {
                ready.fetch_add(1, std::memory_order_release);
                while (!start_flag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < OPS_PER_THREAD; ++i) {
                    Task* task = shared_pool.acquire();
                    shared_pool.release(task);
                } });
        }

        while (ready.load(std::memory_order_acquire) < thread_count)
        {
            std::this_thread::yield();
        }
        start_flag.store(true, std::memory_order_release);

        for (auto &th : threads)
        {
            th.join();
        }

        auto end = Clock::now();
        double sec = std::chrono::duration<double>(end - start).count();
        long total_ops = static_cast<long>(thread_count * OPS_PER_THREAD);
        std::cout << "  " << thread_count << " threads: "
                  << static_cast<long>(total_ops / sec)
                  << " ops/sec  ("
                  << static_cast<long>(total_ops / sec / thread_count)
                  << " ops/sec/thread)\n";
    }
}

/* =====================================================================
 * Thread-local pools (no contention)
 * ===================================================================== */

static void test_thread_local()
{
    std::cout << "\n---------- Thread-Local Pool (no contention) ----------\n";

    for (int thread_count : {1, 2, 4, 8})
    {
        constexpr int OPS_PER_THREAD = 1000000;

        std::atomic<int> ready{0};
        std::atomic<bool> start_flag{false};

        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        auto start = Clock::now();

        for (int t = 0; t < thread_count; ++t)
        {
            threads.emplace_back([&]()
                                 {
                MemoryPool local_pool(64);
                ready.fetch_add(1, std::memory_order_release);
                while (!start_flag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < OPS_PER_THREAD; ++i) {
                    Task* task = local_pool.acquire();
                    local_pool.release(task);
                } });
        }

        while (ready.load(std::memory_order_acquire) < thread_count)
        {
            std::this_thread::yield();
        }
        start_flag.store(true, std::memory_order_release);

        for (auto &th : threads)
        {
            th.join();
        }

        auto end = Clock::now();
        double sec = std::chrono::duration<double>(end - start).count();
        long total_ops = static_cast<long>(thread_count * OPS_PER_THREAD);
        std::cout << "  " << thread_count << " threads: "
                  << static_cast<long>(total_ops / sec)
                  << " ops/sec  ("
                  << static_cast<long>(total_ops / sec / thread_count)
                  << " ops/sec/thread)\n";
    }
}

static void bench_grow_policy()
{
    std::cout << "\n---------- Grow policy: Reject vs Expand ----------\n";
    constexpr int OPS = 65536;

    {
        MemoryPool pool(OPS, false);
        std::vector<Task*> held;
        held.reserve(OPS);
        auto start = Clock::now();
        for (int i = 0; i < OPS; ++i) {
            Task* t = pool.acquire();
            if (!t) {
                throw std::runtime_error("reject pool unexpectedly exhausted");
            }
            held.push_back(t);
        }
        for (Task* task : held) {
            pool.release(task);
        }
        auto end = Clock::now();
        std::cout << "  Reject   : " << static_cast<long>(2.0 * OPS / std::chrono::duration<double>(end - start).count())
                  << " ops/sec (capacity=" << pool.capacity() << ")\n";
    }

    {
        MemoryPool pool(1, true);
        std::vector<Task*> held;
        held.reserve(OPS);
        auto start = Clock::now();
        for (int i = 0; i < OPS; ++i) {
            Task* t = pool.acquire();
            if (!t) {
                throw std::runtime_error("expand pool returned nullptr");
            }
            held.push_back(t);
        }
        for (Task* task : held) {
            pool.release(task);
        }
        auto end = Clock::now();
        std::cout << "  Expand   : " << static_cast<long>(2.0 * OPS / std::chrono::duration<double>(end - start).count())
                  << " ops/sec (capacity=" << pool.capacity() << ")\n";
    }
}

int main()
{
    test_latency();
    test_throughput();
    test_contended();
    test_thread_local();
    bench_grow_policy();
}
