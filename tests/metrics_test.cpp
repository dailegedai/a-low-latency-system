#include "../include/ThreadPool.h"
#include "check.h"

#include <chrono>
#include <iostream>

int main()
{
    std::cout << "========== Metrics Test ==========\n";

    ThreadPool pool(4, 100);

    for (int i = 0; i < 20; ++i) {
        pool.submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });
    }

    // 中途指标：20 个 200ms 任务由 4 workers 执行（~1s），
    // 提交后立即检查时必处于执行中：submitted==20 且未全部完成
    CHECK(pool.getSubmittedTaskCount() == 20);
    CHECK(pool.getCompletedTaskCount() < 20);
    std::cout << "  mid: submitted=" << pool.getSubmittedTaskCount()
              << " completed=" << pool.getCompletedTaskCount() << " PASS\n";

    while (pool.getCompletedTaskCount() < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

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
