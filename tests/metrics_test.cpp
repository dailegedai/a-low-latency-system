#include "../include/ThreadPool.h"
#include "check.h"
#include "test_util.h"

#include <atomic>
#include <iostream>

int main()
{
    std::cout << "========== Metrics Test ==========\n";

    ThreadPool pool(4, 100);

    testutil::Gate gate;
    std::atomic<int> started{0};
    std::atomic<int> completed{0};

    for (int i = 0; i < 20; ++i) {
        pool.submitVoid([&] {
            started.fetch_add(1, std::memory_order_relaxed);
            gate.wait(); // 占住 worker，直到主线程放行（确定性中间态）
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // 提交后立即检查：submitted 已确定 == 20
    CHECK(pool.getSubmittedTaskCount() == 20);

    // 4 个 worker 各领一个任务后阻塞在 gate -> 恰有 4 个 started（确定性，无需 sleep）
    CHECK(testutil::waitUntil([&] { return started.load() == 4; }));
    CHECK(pool.getCompletedTaskCount() == 0);
    CHECK(pool.getBusyWorkerCount() > 0);
    std::cout << "  mid: submitted=" << pool.getSubmittedTaskCount()
              << " completed=" << pool.getCompletedTaskCount() << " PASS\n";

    gate.release();
    CHECK(testutil::waitUntil([&] { return pool.getCompletedTaskCount() == 20; }));

    CHECK(pool.getSubmittedTaskCount() == 20);
    CHECK(pool.getCompletedTaskCount() == 20);
    CHECK(pool.getBusyWorkerCount() == 0);
    CHECK(pool.getQueueSize() == 0);
    CHECK(pool.idle());
    std::cout << "  end: submitted=" << pool.getSubmittedTaskCount()
              << " completed=" << pool.getCompletedTaskCount()
              << " queue=" << pool.getQueueSize() << " PASS\n";

    pool.shutdown();

    std::cout << "\n========== Metrics Test PASSED ==========\n";
    return 0;
}
