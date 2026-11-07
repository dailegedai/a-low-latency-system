#include "../include/ThreadPool.h"
#include "check.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

static void test_throw()
{
    std::cout << "[THROW] overflow throws exception\n";
    ThreadPool pool(1, 1, RejectPolicy::THROW);
    testutil::Gate gate;
    std::atomic<bool> started{false};

    pool.submit([&] {
        started.store(true, std::memory_order_release);
        gate.wait();
    });
    CHECK(testutil::waitUntil([&] {
        return started.load(std::memory_order_acquire);
    }));

    pool.submit([] {});

    bool threw = false;
    try {
        pool.submit([] {});
    } catch (const std::exception& e) {
        threw = true;
        std::cout << "  exception: " << e.what() << "\n";
    }

    CHECK(threw);

    gate.release();
    pool.shutdown();
    std::cout << "  third submit rejected PASS\n";
}

static void test_block()
{
    std::cout << "[BLOCK] waits until space available\n";
    ThreadPool pool(1, 1, RejectPolicy::BLOCK);
    testutil::Gate gate;
    std::atomic<bool> started{false};
    std::atomic<int> done{0};

    pool.submit([&] {
        started.store(true, std::memory_order_release);
        gate.wait();
        done.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(testutil::waitUntil([&] {
        return started.load(std::memory_order_acquire);
    }));

    pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });

    std::atomic<bool> attempted{false};
    std::atomic<bool> returned{false};
    std::thread producer([&] {
        attempted.store(true, std::memory_order_release);
        pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });
        returned.store(true, std::memory_order_release);
    });

    CHECK(testutil::waitUntil([&] {
        return attempted.load(std::memory_order_acquire);
    }));
    CHECK(!testutil::waitUntil([&] {
        return returned.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(50)));

    gate.release();
    producer.join();
    pool.shutdown();
    CHECK(done.load() == 3);
    CHECK(pool.getCompletedTaskCount() == 3);
    std::cout << "  producer blocked, then all 3 tasks executed PASS\n";
}

// 回归：队列满时多个生产者阻塞在 not_full_cv，shutdown 必须唤醒它们，
// 否则 join 永久挂起（曾因 BLOCK 谓词不含 stop 且 shutdown 不通知 not_full_cv 而触发）。
static void test_block_shutdown_no_hang()
{
    std::cout << "[BLOCK] producers blocked on full queue exit on shutdown\n";
    // 0 worker：任务只入队不消费，队列容量 4 填满后，后续 submit 必然阻塞在
    // not_full_cv —— 无需 sleep 即可确定性地构造"生产者阻塞"现场。
    ThreadPool pool(0, 4, RejectPolicy::BLOCK);

    std::atomic<int> attempts{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < 10; ++t) {
        producers.emplace_back([&] {
            try {
                while (true) {
                    attempts.fetch_add(1, std::memory_order_relaxed);
                    pool.submit([]() {});
                }
            } catch (const std::exception&) {
                // shutdown 后 submit 抛异常 -> 生产者正常退出
            }
        });
    }

    // 确定性条件：队列已满(4) 且至少一个生产者发起了第 5 次提交（即被阻塞）
    CHECK(testutil::waitUntil([&] {
        return pool.getSubmittedTaskCount() == 4 && attempts.load() >= 5;
    }));

    pool.shutdown();
    for (auto& th : producers) {
        th.join(); // 若 BLOCK 挂起未修复，此处永久阻塞
    }

    CHECK(attempts.load() > 0);
    std::cout << "  all producers joined (attempts=" << attempts.load() << ") PASS\n";
}

static void test_discard()
{
    std::cout << "[DISCARD] full queue silently discards, no throw\n";
    // 0 个 worker：任务只入队不执行，队列必然保持满，测试确定性可控
    ThreadPool pool(0, 4, RejectPolicy::DISCARD);

    for (int i = 0; i < 4; ++i) {
        pool.submit([]() {}); // 填满容量 4
    }

    // 队列已满，后续提交被静默丢弃：不抛异常、不计入 submitted
    std::vector<std::future<int>> futs;
    for (int i = 0; i < 10; ++i) {
        futs.push_back(pool.submit([]() { return 42; }));
    }

    CHECK(pool.getSubmittedTaskCount() == 4);
    // 被丢弃任务的 future 得到 broken_promise（packaged_task 析构时置位），而非挂起
    for (auto& f : futs) {
        CHECK(f.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    }
    std::cout << "  submitted stays at 4, 10 discarded PASS\n";
    pool.shutdown();
}

static void test_submit_after_shutdown()
{
    std::cout << "[SHUTDOWN] submit after shutdown throws\n";
    ThreadPool pool(2, 4);
    pool.shutdown();

    bool threw = false;
    try {
        pool.submit([]() {});
    } catch (const std::exception&) {
        threw = true;
    }

    CHECK(threw);
    CHECK(pool.isStopping());
    std::cout << "  PASS\n";
}

int main()
{
    std::cout << "========== Reject Policy Test ==========\n\n";

    test_throw();
    std::cout << "\n";
    test_block();
    std::cout << "\n";
    test_block_shutdown_no_hang();
    std::cout << "\n";
    test_discard();
    std::cout << "\n";
    test_submit_after_shutdown();

    std::cout << "\n========== Reject Policy Test PASSED ==========\n";
    return 0;
}
