#include "../include/LockFreeQueue.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// 期望和：t*PER + i 对所有 (t, i) 求和
static long expected_sum(int producers, int per_producer)
{
    long sum = 0;
    for (int t = 0; t < producers; ++t) {
        for (int i = 0; i < per_producer; ++i) {
            sum += static_cast<long>(t) * per_producer + i;
        }
    }
    return sum;
}

// 单线程 FIFO 语义（单生产者单消费者下无锁队列保序）
static bool test_single_thread()
{
    LockFreeQueue<int> q(4);

    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(q.push(3));
    CHECK(q.push(4));
    CHECK(!q.push(5)); // 满

    int x = 0;
    CHECK(q.pop(x) && x == 1);
    CHECK(q.pop(x) && x == 2);

    CHECK(q.push(5));
    CHECK(q.push(6)); // 环绕后复用槽位

    CHECK(q.pop(x) && x == 3);
    CHECK(q.pop(x) && x == 4);
    CHECK(q.pop(x) && x == 5);
    CHECK(q.pop(x) && x == 6);
    CHECK(!q.pop(x)); // 空

    return true;
}

// 多生产者多消费者：无丢无重（和校验）
static bool test_mpmc_consistency()
{
    constexpr int PRODUCERS = 4;
    constexpr int CONSUMERS = 4;
    constexpr int PER = 10000;
    constexpr long TOTAL = PRODUCERS * PER;

    LockFreeQueue<long> q(8192);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<long> popped{0};
    std::atomic<long> sum{0};

    std::vector<std::thread> threads;

    for (int t = 0; t < PRODUCERS; ++t) {
        threads.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < PER; ++i) {
                long v = static_cast<long>(t) * PER + i;
                while (!q.push(std::move(v))) {
                    std::this_thread::yield(); // 满则重试
                }
            }
        });
    }

    for (int c = 0; c < CONSUMERS; ++c) {
        threads.emplace_back([&, c]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            long v;
            while (popped.load(std::memory_order_relaxed) < TOTAL) {
                if (q.pop(v)) {
                    sum.fetch_add(v, std::memory_order_relaxed);
                    popped.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < PRODUCERS + CONSUMERS) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    CHECK(popped.load() == TOTAL);
    CHECK(sum.load() == expected_sum(PRODUCERS, PER));
    return true;
}

int main()
{
    std::cout << "========== Lock-Free Queue Test ==========\n";

    std::cout << "[single-thread] FIFO + wrap-around\n";
    test_single_thread(); // 内部 CHECK 失败即 exit
    std::cout << "  PASS\n";

    std::cout << "[MPMC] 4 producers x 4 consumers, no loss/dup\n";
    test_mpmc_consistency(); // 内部 CHECK 失败即 exit
    std::cout << "  PASS\n";

    std::cout << "\n========== ALL TESTS PASSED ==========\n";
    return 0;
}
