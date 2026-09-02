#include "../include/ThreadPool.h"
#include "../include/WorkStealingThreadPool.h"
#include "benchmark_util.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// =====================================================================
// 阶段 B：ThreadPool（单一全局锁） vs WorkStealingThreadPool（work stealing）
// 对比 threads=1/2/4/8 的提交-执行吞吐。
//
// 设计：
//   - 队列容量对齐：两种池都用 queue_size 参数作为提交缓冲区容量。
//   - sample() 重复采样取 median（每次运行重置 done 计数器，避免 Day31 的坑）。
//   - 两个场景：
//     (a) 空任务：测纯调度/队列开销；
//     (b) 带 sleep 任务：制造负载不均，steal 应体现价值。
// =====================================================================

static constexpr int N_EMPTY = 200000;   // 空任务场景
static constexpr int N_SLEEPY = 20000;   // sleep 任务场景（sleep 使任务更慢，量减小控制总时长）

// ---- 场景 A：空任务吞吐 ----
template <typename Pool>
static void bench_empty(const char* label, int threads, size_t queue_size)
{
    Pool pool(threads, queue_size);
    std::atomic<int64_t> done{0};

    sample(label, [&] {
        done.store(0, std::memory_order_relaxed);
        for (int i = 0; i < N_EMPTY; ++i) {
            pool.submit([&] { done.fetch_add(1, std::memory_order_relaxed); });
        }
        while (done.load(std::memory_order_relaxed) < N_EMPTY) {
            std::this_thread::yield();
        }
    }, /*warmup=*/1, /*repeats=*/3);

    pool.shutdown();
}

// ---- 场景 B：带 sleep 任务（触发 steal / 负载不均） ----
template <typename Pool>
static void bench_sleepy(const char* label, int threads, size_t queue_size)
{
    Pool pool(threads, queue_size);
    std::atomic<int64_t> done{0};

    sample(label, [&] {
        done.store(0, std::memory_order_relaxed);
        for (int i = 0; i < N_SLEEPY; ++i) {
            // 任务按 id 奇偶分配不同 sleep 时长，制造负载不均
            int delay_us = (i % 8 == 0) ? 20 : 1;
            pool.submit([&, delay_us] {
                std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        while (done.load(std::memory_order_relaxed) < N_SLEEPY) {
            std::this_thread::yield();
        }
    }, /*warmup=*/1, /*repeats=*/3);

    pool.shutdown();
}

int main()
{
    std::cout << "========== Work Stealing vs Single-Lock ThreadPool ==========\n";
    std::cout << "scenario A: " << N_EMPTY << " empty tasks; scenario B: " << N_SLEEPY
              << " mixed-sleep tasks\n";
    std::cout << "sample(): warmup=1 repeats=3, reporting median ms (lower is better)\n\n";

    std::cout << "--- Scenario A: empty tasks (pure scheduling) ---\n";
    for (int t : {1, 2, 4, 8}) {
        bench_empty<ThreadPool>(
            ("  ThreadPool threads=" + std::to_string(t)).c_str(), t, 65536);
        bench_empty<WorkStealingThreadPool>(
            ("  WSTPool    threads=" + std::to_string(t)).c_str(), t, 65536);
    }

    std::cout << "\n--- Scenario B: mixed sleep tasks (load imbalance, steal) ---\n";
    for (int t : {1, 2, 4, 8}) {
        bench_sleepy<ThreadPool>(
            ("  ThreadPool threads=" + std::to_string(t)).c_str(), t, 65536);
        bench_sleepy<WorkStealingThreadPool>(
            ("  WSTPool    threads=" + std::to_string(t)).c_str(), t, 65536);
    }

    std::cout << "\n=== Done ===\n";
    return 0;
}
