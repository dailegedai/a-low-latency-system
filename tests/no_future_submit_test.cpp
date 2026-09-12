#include "../include/ThreadPool.h"
#include "../include/WorkStealingThreadPool.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

// ================= ThreadPool =================

// submitVoid：全部执行、计数一致、无失败
static void tp_submit_void()
{
    std::cout << "[ThreadPool] submitVoid all executed\n";
    ThreadPool pool(2, 16);
    std::atomic<int> n{0};
    for (int i = 0; i < 5000; ++i) {
        pool.submitVoid([&n] { n.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown();
    CHECK(n.load() == 5000);
    CHECK(pool.getSubmittedTaskCount() == 5000);
    CHECK(pool.getCompletedTaskCount() == 5000);
    CHECK(pool.getFailedTaskCount() == 0);
    std::cout << "  PASS\n";
}

// submitVoid BLOCK：队列满时阻塞等待，最终全部执行
static void tp_submit_void_block()
{
    std::cout << "[ThreadPool] submitVoid BLOCK drains\n";
    ThreadPool pool(2, 8, RejectPolicy::BLOCK);
    std::atomic<int> n{0};
    constexpr int N = 5000;
    for (int i = 0; i < N; ++i) {
        pool.submitVoid([&n] {
            std::this_thread::sleep_for(std::chrono::microseconds(2));
            n.fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool.shutdown();
    CHECK(n.load() == N);
    std::cout << "  PASS\n";
}

// 异常被 worker 侧捕获并计入 failed，不逃逸、不终止进程
static void tp_failed_count()
{
    std::cout << "[ThreadPool] submitVoid exception -> failed count\n";
    ThreadPool pool(2, 16);
    pool.submitVoid([] { throw std::runtime_error("boom"); });
    pool.submitVoid([] {});
    pool.shutdown();
    CHECK(pool.getFailedTaskCount() == 1);
    CHECK(pool.getCompletedTaskCount() == 2);
    std::cout << "  PASS\n";
}

// trySubmit：满时返回 false，停止后返回 false，有空位返回 true
static void tp_try_submit()
{
    std::cout << "[ThreadPool] trySubmit full/stopped\n";
    ThreadPool pool(1, 1);
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};

    CHECK(pool.trySubmit([&] {
        started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }));
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // worker 忙、队列空：第 2 个进队列（容量 1），第 3 个必失败
    CHECK(pool.trySubmit([] {}));
    CHECK(!pool.trySubmit([] {}));

    release.store(true, std::memory_order_release);
    pool.shutdown();

    CHECK(!pool.trySubmit([] {})); // shutdown 后拒绝
    std::cout << "  PASS\n";
}

// ================= WorkStealingThreadPool =================

static void ws_submit_void()
{
    std::cout << "[WST] submitVoid all executed\n";
    WorkStealingThreadPool pool(4, 64);
    std::atomic<int> n{0};
    for (int i = 0; i < 5000; ++i) {
        pool.submitVoid([&n] { n.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown();
    CHECK(n.load() == 5000);
    CHECK(pool.getSubmittedTaskCount() == 5000);
    CHECK(pool.getCompletedTaskCount() == 5000);
    CHECK(pool.getFailedTaskCount() == 0);
    std::cout << "  PASS\n";
}

static void ws_failed_count()
{
    std::cout << "[WST] submitVoid exception -> failed count\n";
    WorkStealingThreadPool pool(4, 32);
    pool.submitVoid([] { throw std::runtime_error("boom"); });
    pool.submitVoid([] {});
    pool.shutdown();
    CHECK(pool.getFailedTaskCount() == 1);
    CHECK(pool.getCompletedTaskCount() == 2);
    std::cout << "  PASS\n";
}

static void ws_try_submit()
{
    std::cout << "[WST] trySubmit stopped -> false\n";
    WorkStealingThreadPool pool(2, 8);
    pool.shutdown();
    CHECK(!pool.trySubmit([] {}));
    std::cout << "  PASS\n";
}

int main()
{
    std::cout << "========== No-Future Submit Test ==========\n\n";
    tp_submit_void();
    tp_submit_void_block();
    tp_failed_count();
    tp_try_submit();
    ws_submit_void();
    ws_failed_count();
    ws_try_submit();
    std::cout << "\n========== ALL TESTS PASSED ==========\n";
    return 0;
}
