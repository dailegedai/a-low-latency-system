#include "../include/ThreadPool.h"
#include "check.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    std::cout << "========== Bounded Queue Test ==========\n";

    // 一个 worker 被 Gate 占住，队列容量 1：第三个提交必须阻塞。
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

    // 第二个任务进入容量为 1 的队列。
    pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });

    std::atomic<bool> producer_attempted{false};
    std::atomic<bool> producer_returned{false};
    std::thread producer([&] {
        producer_attempted.store(true, std::memory_order_release);
        pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });
        producer_returned.store(true, std::memory_order_release);
    });

    CHECK(testutil::waitUntil([&] {
        return producer_attempted.load(std::memory_order_acquire);
    }));
    // Negative wait: while the only worker is gated, submit must not return.
    CHECK(!testutil::waitUntil([&] {
        return producer_returned.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(50)));

    gate.release();
    producer.join();

    pool.shutdown();

    CHECK(done.load() == 3);
    CHECK(pool.getSubmittedTaskCount() == 3);
    CHECK(pool.getCompletedTaskCount() == 3);

    std::cout << "  submitted=" << pool.getSubmittedTaskCount()
              << " completed=" << pool.getCompletedTaskCount() << " PASS\n";

    std::cout << "========== Bounded Queue Test PASSED ==========\n";
    return 0;
}
