#include "../include/TaskTrace.h"
#include "../include/ThreadPool.h"
#include "../include/WorkStealingThreadPool.h"
#include "check.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

using llengine::TaskTrace;

// 线程安全的采集器（sink 会被多个 worker 并发调用）
struct Collector {
    std::mutex m;
    std::vector<TaskTrace> traces;

    void operator()(const TaskTrace& t)
    {
        std::lock_guard<std::mutex> lock(m);
        traces.push_back(t);
    }
    size_t size()
    {
        std::lock_guard<std::mutex> lock(m);
        return traces.size();
    }
    std::vector<TaskTrace> snapshot()
    {
        std::lock_guard<std::mutex> lock(m);
        return traces;
    }
};

static void check_traces_valid(const std::vector<TaskTrace>& v)
{
    // 注意：任务并发执行，sink 回调顺序 ≠ 提交顺序，故不能断言采集顺序上 id 单调。
    // 只校验：id 唯一（全局生成器单调保证）、完成不早于提交、时间戳有效。
    std::unordered_set<uint64_t> ids;
    for (const auto& t : v) {
        CHECK(ids.insert(t.id).second);       // id 唯一
        CHECK(t.complete_ns >= t.submit_ns);  // 完成不早于提交
        CHECK(t.submit_ns > 0);
    }
}

// TEST1: nextTaskId 单调递增
static void test_id_monotonic()
{
    std::cout << "[id] monotonic\n";
    const int N = 100000;
    uint64_t prev = llengine::nextTaskId();
    for (int i = 0; i < N; ++i) {
        const uint64_t cur = llengine::nextTaskId();
        CHECK(cur > prev);
        prev = cur;
    }
    std::cout << "  PASS\n";
}

// TEST2: ThreadPool submitVoid 全量追踪
static void test_tp_submit_void_traced()
{
    std::cout << "[ThreadPool] submitVoid traced\n";
    ThreadPool pool(4, 64);
    auto col = std::make_shared<Collector>();
    pool.setTraceSink([col](const TaskTrace& t) { (*col)(t); });

    const int N = 5000;
    std::atomic<int> done{0};
    for (int i = 0; i < N; ++i) {
        pool.submitVoid([&done] { done.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown();

    CHECK(done.load() == N);
    CHECK(col->size() == static_cast<size_t>(N));
    check_traces_valid(col->snapshot());
    std::cout << "  PASS\n";
}

// TEST3: ThreadPool submit(future) 也被追踪
static void test_tp_submit_future_traced()
{
    std::cout << "[ThreadPool] submit(future) traced\n";
    ThreadPool pool(2, 32);
    auto col = std::make_shared<Collector>();
    pool.setTraceSink([col](const TaskTrace& t) { (*col)(t); });

    const int N = 1000;
    std::vector<std::future<int>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        futs.push_back(pool.submit([i] { return i * 2; }));
    }
    for (int i = 0; i < N; ++i) {
        CHECK(futs[i].get() == i * 2);
    }
    pool.shutdown();

    CHECK(col->size() == static_cast<size_t>(N));
    check_traces_valid(col->snapshot());
    std::cout << "  PASS\n";
}

// TEST4: 异常任务也被追踪，且计入 failed
static void test_tp_failed_traced()
{
    std::cout << "[ThreadPool] failed task traced\n";
    ThreadPool pool(2, 16);
    auto col = std::make_shared<Collector>();
    pool.setTraceSink([col](const TaskTrace& t) { (*col)(t); });

    pool.submitVoid([] { throw std::runtime_error("boom"); });
    pool.submitVoid([] {});
    pool.shutdown();

    CHECK(pool.getFailedTaskCount() == 1);
    CHECK(col->size() == 2);
    check_traces_valid(col->snapshot());
    std::cout << "  PASS\n";
}

// TEST5: 关闭/切换 sink 语义（按提交时刻生效）
static void test_tp_sink_switch()
{
    std::cout << "[ThreadPool] sink switch / disable\n";
    ThreadPool pool(2, 32);
    auto col1 = std::make_shared<Collector>();
    auto col2 = std::make_shared<Collector>();

    pool.setTraceSink([col1](const TaskTrace& t) { (*col1)(t); });
    pool.submitVoid([] {});
    pool.setTraceSink(llengine::TaskTraceSink{}); // 关闭
    pool.submitVoid([] {});
    pool.setTraceSink([col2](const TaskTrace& t) { (*col2)(t); });
    pool.submitVoid([] {});
    pool.shutdown();

    CHECK(col1->size() == 1);
    CHECK(col2->size() == 1);
    std::cout << "  PASS\n";
}

// TEST6: trySubmit 只追踪成功入队的任务
static void test_tp_try_submit_traced()
{
    std::cout << "[ThreadPool] trySubmit traced (successful only)\n";
    ThreadPool pool(1, 1);
    auto col = std::make_shared<Collector>();
    pool.setTraceSink([col](const TaskTrace& t) { (*col)(t); });

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
    CHECK(pool.trySubmit([] {}));   // 第二个成功（队列空）
    CHECK(!pool.trySubmit([] {}));  // 第三个满，拒绝

    release.store(true, std::memory_order_release);
    pool.shutdown();
    CHECK(col->size() == 2); // 只有成功的两个被执行并回调
    std::cout << "  PASS\n";
}

// TEST7: WorkStealingThreadPool 追踪
static void test_wst_traced()
{
    std::cout << "[WST] submitVoid traced\n";
    WorkStealingThreadPool pool(4, 64);
    auto col = std::make_shared<Collector>();
    pool.setTraceSink([col](const TaskTrace& t) { (*col)(t); });

    const int N = 5000;
    std::atomic<int> done{0};
    for (int i = 0; i < N; ++i) {
        pool.submitVoid([&done] { done.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown();

    CHECK(done.load() == N);
    CHECK(col->size() == static_cast<size_t>(N));
    check_traces_valid(col->snapshot());
    std::cout << "  PASS\n";
}

// TEST8: 默认关闭时无额外行为（功能正常、无崩溃）
static void test_default_disabled()
{
    std::cout << "[ThreadPool] tracing disabled by default\n";
    ThreadPool pool(2, 16);
    std::atomic<int> done{0};
    for (int i = 0; i < 200; ++i) {
        pool.submitVoid([&done] { done.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown();
    CHECK(done.load() == 200);
    CHECK(pool.getCompletedTaskCount() == 200);
    std::cout << "  PASS\n";
}

int main()
{
    std::cout << "========== TaskTrace Test ==========\n\n";
    test_id_monotonic();
    test_tp_submit_void_traced();
    test_tp_submit_future_traced();
    test_tp_failed_traced();
    test_tp_sink_switch();
    test_tp_try_submit_traced();
    test_wst_traced();
    test_default_disabled();
    std::cout << "\n========== ALL TESTS PASSED ==========\n";
    return 0;
}
