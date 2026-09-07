#include "../include/WorkStealingQueue.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using llengine::QueueWorker;
using llengine::QueueStealer;

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

// owner 单线程 push/pop：LIFO
static bool test_owner_lifo()
{
    QueueWorker<int> wk(4);
    CHECK(wk.empty());
    CHECK(wk.push(1));
    CHECK(wk.push(2));
    CHECK(wk.push(3));

    int x = 0;
    CHECK(wk.pop(x) && x == 3); // LIFO
    CHECK(wk.pop(x) && x == 2);
    CHECK(wk.pop(x) && x == 1);
    CHECK(!wk.pop(x)); // 空
    CHECK(wk.size() == 0);

    return true;
}

// 满时 push 返回 false
static bool test_full_rejects()
{
    QueueWorker<int> wk(2);
    CHECK(wk.push(1));
    CHECK(wk.push(2));
    CHECK(!wk.push(3)); // 满

    int x = 0;
    CHECK(wk.pop(x) && x == 2);
    CHECK(wk.push(3)); // 空出后可再 push
    CHECK(wk.pop(x) && x == 3);
    CHECK(wk.pop(x) && x == 1);
    return true;
}

// owner 与 thief 竞争最后一个元素：只应一方取到
static bool test_last_element_race()
{
    for (int round = 0; round < 100; ++round) {
        QueueWorker<int> wk(4);
        auto st = wk.make_stealer();
        wk.push(42);

        std::atomic<int> taken{0};
        std::thread thief([&] {
            int x = 0;
            if (st.steal(x)) taken.fetch_add(1);
        });
        int x = 0;
        if (wk.pop(x)) taken.fetch_add(1);
        thief.join();

        CHECK(taken.load() == 1); // 恰好一方取到
    }
    return true;
}

// 多 deque × 多 thief：无丢无重（和校验）。
// 每 deque 一个 Worker（owner 只 push，满则自旋），多 thief 用各自 Stealer 并发 steal。
static bool test_multithread_steal()
{
    constexpr int OWNERS = 4;
    constexpr int THIEVES = 4;
    constexpr int PER = 10000;
    constexpr long TOTAL = OWNERS * PER;

    // Worker 不可拷贝，用 unique_ptr 持有（每 deque 一个）
    std::vector<std::unique_ptr<QueueWorker<long>>> workers;
    for (int i = 0; i < OWNERS; ++i) {
        workers.push_back(std::make_unique<QueueWorker<long>>(PER + 64));
    }

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<long> popped{0};
    std::atomic<long> sum{0};

    std::vector<std::thread> threads;

    for (int t = 0; t < OWNERS; ++t) {
        threads.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < PER; ++i) {
                long v = static_cast<long>(t) * PER + i;
                while (!workers[t]->push(v)) {
                    std::this_thread::yield(); // 满则重试
                }
            }
        });
    }

    for (int c = 0; c < THIEVES; ++c) {
        threads.emplace_back([&, c]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // 每个 thief 持有所有 deque 的 Stealer
            std::vector<QueueStealer<long>> stealers;
            for (int i = 0; i < OWNERS; ++i) {
                stealers.push_back(workers[i]->make_stealer());
            }
            long v;
            while (popped.load(std::memory_order_relaxed) < TOTAL) {
                for (size_t d = 0; d < stealers.size(); ++d) {
                    if (stealers[d].steal(v)) {
                        sum.fetch_add(v, std::memory_order_relaxed);
                        popped.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                std::this_thread::yield();
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < OWNERS + THIEVES) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    CHECK(popped.load() == TOTAL);
    CHECK(sum.load() == expected_sum(OWNERS, PER));
    return true;
}

// owner 本地取一部分 + thief 偷取其余：无丢无重（含槽位环绕）
static bool test_mixed_pop_steal()
{
    constexpr int OWNERS = 2;
    constexpr int PER = 20000;
    constexpr long TOTAL = OWNERS * PER;

    std::vector<std::unique_ptr<QueueWorker<long>>> workers;
    for (int i = 0; i < OWNERS; ++i) {
        workers.push_back(std::make_unique<QueueWorker<long>>(PER + 64));
    }

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<long> popped{0};
    std::atomic<long> sum{0};

    std::vector<std::thread> threads;

    for (int t = 0; t < OWNERS; ++t) {
        threads.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < PER; ++i) {
                long v = static_cast<long>(t) * PER + i;
                while (!workers[t]->push(v)) {
                    std::this_thread::yield();
                }
            }
            // owner 本地 LIFO 取一半
            long x;
            for (int i = 0; i < PER / 2; ++i) {
                if (workers[t]->pop(x)) {
                    sum.fetch_add(x, std::memory_order_relaxed);
                    popped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::thread thief([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::vector<QueueStealer<long>> stealers;
        for (int i = 0; i < OWNERS; ++i) {
            stealers.push_back(workers[i]->make_stealer());
        }
        long v;
        while (popped.load(std::memory_order_relaxed) < TOTAL) {
            for (size_t d = 0; d < stealers.size(); ++d) {
                if (stealers[d].steal(v)) {
                    sum.fetch_add(v, std::memory_order_relaxed);
                    popped.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            std::this_thread::yield();
        }
    });

    while (ready.load(std::memory_order_acquire) < OWNERS + 1) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }
    thief.join();

    CHECK(popped.load() == TOTAL);
    CHECK(sum.load() == expected_sum(OWNERS, PER));
    return true;
}

int main()
{
    std::cout << "========== Work Stealing Queue (V2) Test ==========\n";

    std::cout << "[owner] LIFO push/pop\n";
    test_owner_lifo(); // 内部 CHECK 失败即 exit
    std::cout << "  PASS\n";

    std::cout << "[full] push returns false when full\n";
    test_full_rejects();
    std::cout << "  PASS\n";

    std::cout << "[race] last element owner-vs-thief\n";
    test_last_element_race();
    std::cout << "  PASS\n";

    std::cout << "[MPMC] " << 4 << " workers x " << 4 << " thieves, no loss/dup\n";
    test_multithread_steal();
    std::cout << "  PASS\n";

    std::cout << "[mixed] owner pop + thief steal, no loss/dup (wrap-around)\n";
    test_mixed_pop_steal();
    std::cout << "  PASS\n";

    std::cout << "\n========== ALL TESTS PASSED ==========\n";
    return 0;
}
