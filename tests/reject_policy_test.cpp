#include "../include/ThreadPool.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

static void test_throw()
{
    std::cout << "[THROW] overflow throws exception\n";
    ThreadPool pool(2, 4, RejectPolicy::THROW);

    int submitted = 0;
    try {
        for (int i = 0; i < 100; ++i) {
            pool.submit([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            });
            ++submitted;
        }
    } catch (const std::exception& e) {
        std::cout << "  exception: " << e.what() << "\n";
    }

    CHECK(submitted < 100);
    std::cout << "  submitted before throw: " << submitted << " PASS\n";
    pool.shutdown();
}

static void test_block()
{
    std::cout << "[BLOCK] waits until space available\n";
    ThreadPool pool(2, 4, RejectPolicy::BLOCK);

    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        pool.submit([]() {});
    }

    pool.shutdown();
    CHECK(pool.getCompletedTaskCount() == N);
    std::cout << "  all " << N << " tasks executed PASS\n";
}

// 回归：队列满时多个生产者阻塞在 not_full_cv，shutdown 必须唤醒它们，
// 否则 join 永久挂起（曾因 BLOCK 谓词不含 stop 且 shutdown 不通知 not_full_cv 而触发）。
static void test_block_shutdown_no_hang()
{
    std::cout << "[BLOCK] producers blocked on full queue exit on shutdown\n";
    ThreadPool pool(2, 4, RejectPolicy::BLOCK);

    std::atomic<int> submitted{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < 10; ++t) {
        producers.emplace_back([&] {
            try {
                while (true) {
                    pool.submit([]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    });
                    submitted.fetch_add(1);
                }
            } catch (const std::exception&) {
                // shutdown 后 submit 抛异常 -> 生产者正常退出
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // 让队列充满、生产者阻塞
    pool.shutdown();
    for (auto& th : producers) {
        th.join(); // 若 BLOCK 挂起未修复，此处永久阻塞
    }

    CHECK(submitted.load() > 0);
    std::cout << "  all producers joined (submitted=" << submitted.load() << ") PASS\n";
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
