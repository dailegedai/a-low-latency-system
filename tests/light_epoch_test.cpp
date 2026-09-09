#include "../include/LightEpoch.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using llengine::LightEpoch;

// 模拟临界区的"活跃计数"：thief enter 后自增、exit 前自减。
// owner 在 QuiesceWindow 内断言该计数必为 0 —— 证明无 thief 在临界区。
static std::atomic<int> g_in_critical{0};
static std::atomic<long> g_total_critical_entries{0};
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_violation{false};

// thief 循环：enter → 进入临界区（inc + 短暂 busy work）→ exit
static void thief_loop(LightEpoch& epoch)
{
    while (!g_stop.load(std::memory_order_acquire)) {
        LightEpoch::Guard guard(epoch);
        int now = g_in_critical.fetch_add(1) + 1;
        (void)now;
        // 用 volatile 忙等制造一个真实临界区窗口（比纯 inc 更能暴露 race）
        volatile long acc = 0;
        for (int i = 0; i < 100; ++i) acc += i;
        (void)acc;
        g_in_critical.fetch_sub(1);
        g_total_critical_entries.fetch_add(1, std::memory_order_relaxed);
    }
}

int main()
{
    std::cout << "========== LightEpoch Test ==========\n";

    LightEpoch epoch; // 单个实例（模拟一个 deque 的 epoch）

    constexpr int N_THIEVES = 6;
    std::vector<std::thread> thieves;
    for (int i = 0; i < N_THIEVES; ++i) {
        thieves.emplace_back(thief_loop, std::ref(epoch));
    }

    // owner：周期性进入 QuiesceWindow，断言无 thief 在临界区
    constexpr int N_WINDOWS = 2000;
    for (int w = 0; w < N_WINDOWS; ++w) {
        LightEpoch::QuiesceWindow window(epoch);
        // 窗口内应无活跃临界区
        if (g_in_critical.load(std::memory_order_relaxed) != 0) {
            g_violation.store(true, std::memory_order_relaxed);
            break;
        }
        // 短暂忙等，扩大 owner 在窗口内停留的时间，提高 race 暴露概率
        volatile long acc = 0;
        for (int i = 0; i < 200; ++i) acc += i;
        (void)acc;
    }

    g_stop.store(true, std::memory_order_release);
    for (auto& th : thieves) {
        th.join();
    }

    CHECK(!g_violation.load());
    CHECK(g_total_critical_entries.load() > 0);
    std::cout << "  windows=2000, critical_entries="
              << g_total_critical_entries.load() << " PASS\n";

    std::cout << "\n========== ALL TESTS PASSED ==========\n";
    return 0;
}
