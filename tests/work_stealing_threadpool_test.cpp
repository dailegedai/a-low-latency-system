#include "../include/WorkStealingThreadPool.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// TEST1: 正常提交执行，future 值正确
static void test_basic()
{
    std::cout << "[basic] submit + future\n";
    {
        WorkStealingThreadPool pool(4, 64);
        auto fut1 = pool.submit([]() { return 42; });
        auto fut2 = pool.submit([](int a, int b) { return a + b; }, 3, 4);
        auto fut3 = pool.submit([]() { return std::string("hi"); });

        CHECK(fut1.get() == 42);
        CHECK(fut2.get() == 7);
        CHECK(fut3.get() == "hi");
    }
    std::cout << "  PASS\n";
}

// TEST2: 批量任务全部执行，无丢失
static void test_bulk()
{
    std::cout << "[bulk] 100k tasks all executed\n";
    {
        WorkStealingThreadPool pool(8, 256);
        std::atomic<long> counter{0};

        for (int i = 0; i < 100000; ++i) {
            pool.submit([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
        }

        pool.shutdown();
        CHECK(counter.load() == 100000);
        CHECK(pool.getSubmittedTaskCount() == 100000);
        CHECK(pool.getCompletedTaskCount() == 100000);
    }
    std::cout << "  PASS\n";
}

// TEST3: 异常传播到 future
static void test_exception()
{
    std::cout << "[exception] propagates to future\n";
    {
        WorkStealingThreadPool pool(4, 32);
        auto fut = pool.submit([]() -> int { throw std::runtime_error("boom"); });
        bool caught = false;
        try {
            fut.get();
        } catch (const std::runtime_error&) {
            caught = true;
        }
        CHECK(caught);
    }
    std::cout << "  PASS\n";
}

// TEST4: shutdown 后 submit 抛异常
static void test_submit_after_shutdown()
{
    std::cout << "[shutdown] submit after shutdown throws\n";
    {
        WorkStealingThreadPool pool(4, 32);
        pool.shutdown();

        bool threw = false;
        try {
            pool.submit([]() {});
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(pool.isStopping());
    }
    std::cout << "  PASS\n";
}

// TEST5: 并发 submit + shutdown 生命周期安全（无挂起、无崩溃）
static void test_concurrent_submit_shutdown()
{
    std::cout << "[lifetime] concurrent submit + shutdown\n";
    std::atomic<bool> stop_submit{false};
    std::atomic<int> completed{0};

    {
        WorkStealingThreadPool pool(1, 2, WorkStealingRejectPolicy::BLOCK);

        std::vector<std::thread> producers;
        for (int t = 0; t < 8; ++t) {
            producers.emplace_back([&] {
                try {
                    while (!stop_submit.load(std::memory_order_relaxed)) {
                        auto fut = pool.submit([&] {
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                            completed.fetch_add(1, std::memory_order_relaxed);
                        });
                        (void)fut;
                    }
                } catch (const std::exception&) {
                    // shutdown 后提交抛异常：预期内
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pool.shutdown();
        stop_submit.store(true);
        for (auto& th : producers) th.join();
    } // 析构

    CHECK(completed.load() > 0);
    std::cout << "  PASS\n";
}

// TEST6: BLOCK 策略 —— 队列满时阻塞等待，最终全部执行
static void test_block_policy()
{
    std::cout << "[BLOCK] waits for space, all executed\n";
    {
        WorkStealingThreadPool pool(2, 8, WorkStealingRejectPolicy::BLOCK);
        std::atomic<int> done{0};
        constexpr int N = 5000;
        for (int i = 0; i < N; ++i) {
            pool.submit([&done]() {
                std::this_thread::sleep_for(std::chrono::microseconds(5));
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        pool.shutdown();
        CHECK(done.load() == N);
    }
    std::cout << "  PASS\n";
}

// TEST7: THROW 策略 —— 队列满抛异常，提交数 < N
static void test_throw_policy()
{
    std::cout << "[THROW] throws when full\n";
    {
        WorkStealingThreadPool pool(1, 4, WorkStealingRejectPolicy::THROW);
        int submitted = 0;
        try {
            for (int i = 0; i < 1000; ++i) {
                pool.submit([]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                });
                ++submitted;
            }
        } catch (const std::exception&) {
        }
        CHECK(submitted < 1000);
    }
    std::cout << "  PASS\n";
}

// TEST8: DISCARD 策略 —— 满时静默丢弃，不抛异常、不计入 submitted
static void test_discard_policy()
{
    std::cout << "[DISCARD] silently discards, no throw\n";
    {
        WorkStealingThreadPool pool(1, 4, WorkStealingRejectPolicy::DISCARD);
        std::atomic<int> done{0};

        // 填满入口（worker 在跑，入口容量 4）
        for (int i = 0; i < 100; ++i) {
            pool.submit([&done]() {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        pool.shutdown();

        CHECK(done.load() > 0);               // 至少执行了部分
        CHECK(pool.getSubmittedTaskCount() < 100); // 有丢弃
    }
    std::cout << "  PASS\n";
}

// TEST9: metrics —— submitted/completed/busy 一致性
static void test_metrics()
{
    std::cout << "[metrics] counters consistent\n";
    {
        WorkStealingThreadPool pool(4, 64);
        std::atomic<int> done{0};
        for (int i = 0; i < 200; ++i) {
            pool.submit([&done]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        while (pool.getCompletedTaskCount() < 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(pool.getSubmittedTaskCount() == 200);
        CHECK(pool.getCompletedTaskCount() == 200);
        CHECK(pool.getBusyWorkerCount() == 0);
        CHECK(pool.idle());
        pool.shutdown();
    }
    std::cout << "  PASS\n";
}

int main()
{
    std::cout << "========== Work Stealing ThreadPool Test ==========\n\n";

    test_basic();
    test_bulk();
    test_exception();
    test_submit_after_shutdown();
    test_concurrent_submit_shutdown();
    test_block_policy();
    test_throw_policy();
    test_discard_policy();
    test_metrics();

    std::cout << "\n========== ALL TESTS PASSED ==========\n";
    return 0;
}
