#include "../include/WorkStealingDeque.h"   // V1
#include "../include/WorkStealingQueue.h"   // V2 (QueueWorker/QueueStealer)
#include "benchmark_util.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// =====================================================================
// Phase 4：V1 (WorkStealingDeque, bottom 原子) vs V2 (WorkStealingQueue,
// bottom 普通 int64 + 权限隔离) deque 级吞吐对比。
//
// 同构场景：OWNERS 个 deque，各由 owner 线程 push；THIEVES 个 thief
// 线程并发 steal 全部 deque。测"提交+窃取"整体耗时（sample 每次完整重建）。
//
// 公平性说明：
//   - V1 push 满返回 false（需调用方重试）；V2 push 满自动 grow。
//   - 容量设足够大使 V1 不频繁满、V2 不频繁 grow —— 都测正常热路径。
//   - V2 的 steal 进 per-deque epoch guard（Phase 2/3 引入的真实开销），
//     予以如实测量。
//   - V1 为固定容量、无 grow/epoch 开销的"下限参照"；V2 额外承担
//     epoch guard + 数组间接访问（array 指针 vs V1 直存 vector）。
// =====================================================================

static constexpr int OWNERS = 4;
static constexpr int THIEVES = 4;
static constexpr int PER = 100000;
static constexpr long TOTAL = OWNERS * PER;

using llengine::QueueWorker;
using llengine::QueueStealer;

// ---- V1：owner push（满则自旋重试）；thief steal ----
static void run_v1_once()
{
    std::vector<std::unique_ptr<WorkStealingDeque<long>>> deques;
    for (int i = 0; i < OWNERS; ++i) {
        deques.push_back(std::make_unique<WorkStealingDeque<long>>(PER + 64));
    }
    std::atomic<long> popped{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> ts;
    for (int t = 0; t < OWNERS; ++t) {
        ts.emplace_back([&, t] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < PER; ++i) {
                long v = static_cast<long>(t) * PER + i;
                while (!deques[t]->push(v)) std::this_thread::yield();
            }
        });
    }
    for (int c = 0; c < THIEVES; ++c) {
        ts.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            long v;
            while (popped.load(std::memory_order_relaxed) < TOTAL) {
                for (auto& dq : deques) {
                    if (dq->steal(v)) { popped.fetch_add(1, std::memory_order_relaxed); break; }
                }
                std::this_thread::yield();
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
}

// ---- V2：owner push（满自动 grow）；thief 用 Stealer ----
static void run_v2_once()
{
    std::vector<std::unique_ptr<QueueWorker<long>>> workers;
    for (int i = 0; i < OWNERS; ++i) {
        workers.push_back(std::make_unique<QueueWorker<long>>(PER + 64));
    }
    std::atomic<long> popped{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> ts;
    for (int t = 0; t < OWNERS; ++t) {
        ts.emplace_back([&, t] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < PER; ++i) {
                long v = static_cast<long>(t) * PER + i;
                workers[t]->push(v); // grow 后恒成功
            }
        });
    }
    for (int c = 0; c < THIEVES; ++c) {
        ts.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            std::vector<QueueStealer<long>> stealers;
            for (auto& w : workers) stealers.push_back(w->make_stealer());
            long v;
            while (popped.load(std::memory_order_relaxed) < TOTAL) {
                for (auto& s : stealers) {
                    if (s.steal(v)) { popped.fetch_add(1, std::memory_order_relaxed); break; }
                }
                std::this_thread::yield();
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
}

int main()
{
    std::cout << "========== V1 vs V2 Deque Benchmark ==========\n";

    std::cout << "--- Scenario 1: pure owner push (N threads, no steal) ---\n";
    std::cout << "  (隔离测 push 热路径 —— V2 bottom 去原子的核心收益区)\n";
    sample("V1 (WorkStealingDeque, bottom atomic)", [] {
        std::vector<std::unique_ptr<WorkStealingDeque<long>>> deques;
        for (int i = 0; i < OWNERS; ++i)
            deques.push_back(std::make_unique<WorkStealingDeque<long>>(PER + 64));
        std::atomic<bool> start{false};
        std::vector<std::thread> ts;
        for (int t = 0; t < OWNERS; ++t)
            ts.emplace_back([&, t] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < PER; ++i) {
                    long v = static_cast<long>(t) * PER + i;
                    while (!deques[t]->push(v)) std::this_thread::yield();
                }
            });
        start.store(true, std::memory_order_release);
        for (auto& th : ts) th.join();
    }, 1, 3);

    sample("V2 (WorkStealingQueue,  bottom plain)", [] {
        std::vector<std::unique_ptr<QueueWorker<long>>> workers;
        for (int i = 0; i < OWNERS; ++i)
            workers.push_back(std::make_unique<QueueWorker<long>>(PER + 64));
        std::atomic<bool> start{false};
        std::vector<std::thread> ts;
        for (int t = 0; t < OWNERS; ++t)
            ts.emplace_back([&, t] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < PER; ++i) {
                    long v = static_cast<long>(t) * PER + i;
                    workers[t]->push(v);
                }
            });
        start.store(true, std::memory_order_release);
        for (auto& th : ts) th.join();
    }, 1, 3);

    std::cout << "\n--- Scenario 2: owner push + thief steal ---\n";
    sample("V1 (WorkStealingDeque, bottom atomic)", run_v1_once, 1, 3);
    sample("V2 (WorkStealingQueue,  bottom plain)", run_v2_once, 1, 3);

    std::cout << "\n=== Done ===\n";
    return 0;
}
