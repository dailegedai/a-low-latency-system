#include "../include/ThreadPool.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <iostream>

int main()
{
    std::cout << "========== Bounded Queue Test ==========\n";

    // 队列容量 5、2 个 worker、BLOCK 策略：提交 20 个慢任务，
    // 验证提交在队列满时阻塞等待（而非丢弃/抛异常），且全部执行。
    ThreadPool pool(2, 5, RejectPolicy::BLOCK);
    std::atomic<int> done{0};

    for (int i = 0; i < 20; i++) {
        pool.submit([&done] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            done.fetch_add(1);
        });
    }

    pool.shutdown();

    CHECK(done.load() == 20);
    CHECK(pool.getSubmittedTaskCount() == 20);
    CHECK(pool.getCompletedTaskCount() == 20);

    std::cout << "  submitted=" << pool.getSubmittedTaskCount()
              << " completed=" << pool.getCompletedTaskCount() << " PASS\n";

    std::cout << "========== Bounded Queue Test PASSED ==========\n";
    return 0;
}
