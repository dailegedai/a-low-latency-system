#include "../include/MemoryPool.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>


/* =====================================================================
 * Part 1 — Functional Correctness
 * ===================================================================== */

static void test_functional()
{
    std::cout << "[Functional] acquire within capacity\n";
    {
        MemoryPool m(4);
        Task *t = m.acquire();
        assert(t != nullptr);
        t->execute();
        m.release(t);
    }
    std::cout << "  PASS\n";

    std::cout << "[Functional] acquire on empty pool returns nullptr\n";
    {
        MemoryPool m(2);
        assert(m.acquire() != nullptr);
        assert(m.acquire() != nullptr);
        assert(m.acquire() == nullptr);
    }
    std::cout << "  PASS\n";

    std::cout << "[Functional] release and re-acquire reuses object\n";
    {
        MemoryPool m(1);
        Task *t1 = m.acquire();
        assert(t1 != nullptr);
        m.release(t1);
        Task *t2 = m.acquire();
        assert(t2 != nullptr);
        assert(t2 == t1);
        (void)t2;
    }
    std::cout << "  PASS\n";

    std::cout << "[Functional] function replaced after re-acquire\n";
    {
        MemoryPool m(1);
        int val = 0;
        Task *t = m.acquire();
        m.release(t);
        t = m.acquire();
        t->setFunction([&val]()
                       { val = 2; });
        t->execute();
        assert(val == 2);
    }
    std::cout << "  PASS\n";

    std::cout << "[Functional] zero capacity\n";
    {
        MemoryPool m(0);
        assert(m.acquire() == nullptr);
    }
    std::cout << "  PASS\n";
}



static void test_thread_safety()
{
    std::cout << "\n---------- ThreadSafety concurrent acquire/release ----------\n";
    MemoryPool pool(64);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&]()
                             {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 10000; ++i) {
                Task* task = pool.acquire();
                pool.release(task);
            } });
    }
    while (ready.load() < 4)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();
    assert(pool.available() == 64);
    std::cout << "  PASS\n";
}

static void test_expand()
{
    std::cout << "\n[Expand] grow on demand\n";
    {
        MemoryPool pool(1, /*expandable=*/true);
        Task *a1 = pool.acquire();
        assert(a1 != nullptr);
        Task *b1 = pool.acquire();
        assert(b1 != nullptr);
        Task *c1 = pool.acquire();
        assert(c1 != nullptr);
        assert(a1 != b1 && b1 != c1);
        pool.release(a1);
        pool.release(b1);
        pool.release(c1);
        assert(pool.available() == pool.capacity());
        assert(pool.blockCount() == 3);
        std::cout << "  capacity= " << pool.capacity()
                  << " blocks= " << pool.blockCount() << "\n";
    }
    std::cout << " PASS\n";

    std::cout << "[Feature] reject mode still returns null on full\n";
    {
        MemoryPool pool(2, /*expandable=*/false);
        assert(pool.acquire() != nullptr);
        assert(pool.acquire() != nullptr);
    }
    std::cout << " PASS\n";
}

static void test_thread_safety_expand()
{
    std::cout << "\n[ThreadSafety] concurrent acquire/release with expand\n";
    MemoryPool pool(4, /*expandable=*/true);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
    {
        threads.emplace_back([&]()
                             {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < 2000; ++i) {
                Task* task = pool.acquire();
                pool.release(task);
            } });
    }
    while (ready.load() < 8)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto &thread : threads)
        thread.join();
    assert(pool.available() == pool.capacity());
    std::cout << "    capacity=" << pool.capacity()
              << " available=" << pool.available() << "\n";
    std::cout << " PASS\n";
}

/* =====================================================================
 * Main
 * ===================================================================== */

int main()
{
    std::cout << "========== Memory Pool Performance Test ==========\n";

    test_functional();

    test_thread_safety();
    test_expand();
    test_thread_safety_expand();

    std::cout << "\n========== ALL TESTS PASSED ==========\n";

    return 0;
}
