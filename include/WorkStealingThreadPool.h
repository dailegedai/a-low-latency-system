#pragma once

#include "LockFreeQueue.h"
#include "Task.h"
#include "TaskTrace.h"
#include "Worker.h"
#include "WorkStealingDeque.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// =====================================================================
// WorkStealingThreadPool：基于 work-stealing 的线程池。
//
// 架构（对比 ThreadPool 的单一全局 RingBuffer + mtx）：
//   submit() ──▶ LockFreeQueue<Task> 共享入口（MPMC 无锁）
//                  │ worker 从入口批量取任务，push 到自己的私有 deque
//                  ▼
//        deque[0]  deque[1]  ...  deque[N-1]   （WorkStealingDeque<Task>）
//                  │
//        worker[i] 循环：pop 自己(LIFO) → 入口取一批 → steal 别人(FIFO) → 空等 cv
//
// 设计要点：
//   1. 共享入口：submit 由任意线程调用，而 WorkStealingDeque::push 要求单 owner，
//      故任务先入无锁 MPMC 入口，再由 owner worker 转移到自己的 deque。
//   2. 批量转移：worker 一次取一批到本地，减少对共享入口的竞争频率，
//      本地 LIFO 处理有缓存局部性（Chase-Lev 思想）。
//   3. steal：worker 本地空时从别人 deque 头部取（FIFO），实现负载均衡，
//      消除 ThreadPool 的单 mtx 全局锁竞争（Day30 已证其是 threads=4/8 瓶颈）。
//   4. 空闲等待：全局 mtx+cv，worker 全空时阻塞，任务到达/窃取后唤醒。
//   5. 生命周期：与 ThreadPool 一致 —— shutdown() 等待在途 submit 完成后返回。
// =====================================================================

enum class WorkStealingRejectPolicy
{
    BLOCK,
    DISCARD,
    THROW
};

class WorkStealingThreadPool
{
public:
    // num_threads: worker 数；queue_size: 每个 deque 与共享入口的容量
    WorkStealingThreadPool(size_t num_threads, size_t queue_size,
                           WorkStealingRejectPolicy policy = WorkStealingRejectPolicy::BLOCK);
    ~WorkStealingThreadPool();

    template <class F, class... Args>
    auto submit(F &&f, Args &&...args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    // 无 future 的 fire-and-forget 提交：省去 packaged_task/shared_ptr 分配。
    // 遵循池的 RejectPolicy；任务异常由 worker 侧捕获并计入 getFailedTaskCount()。
    template <class F, class... Args>
    void submitVoid(F &&f, Args &&...args);

    // 非阻塞 fire-and-forget：入口满或已停止返回 false，不抛异常。
    template <class F, class... Args>
    bool trySubmit(F &&f, Args &&...args);

    void shutdown();

    // 任务追踪（opt-in）：设置非空 sink 后，每个任务完成时回调一次（含失败任务）。
    // sink 在 worker 线程执行、可能并发，必须线程安全。传空 = 关闭追踪。
    void setTraceSink(llengine::TaskTraceSink sink);

    uint64_t getSubmittedTaskCount() const;
    uint64_t getCompletedTaskCount() const;
    uint64_t getBusyWorkerCount() const;
    // fire-and-forget 任务抛出的异常计数（无 future，无法向调用方传播）
    uint64_t getFailedTaskCount() const;
    size_t getQueueSize();
    bool idle() const;
    size_t getThreadCount() const;
    bool isStopping() const;

private:
    // 入队并应用 RejectPolicy：DISCARD 满返回 false；stop / THROW 抛异常。
    bool enqueueTask(Task &&task);
    // 非阻塞入队：入口满或 stop 返回 false（不抛异常）。
    bool tryEnqueueTask(Task &&task);
    // 若已开启追踪则包裹 body（生成 id + 提交时间戳，完成后回调 sink），否则原样返回。
    Task makeTraceableTask(std::function<void()> body);

    void workerLoop(size_t me);
    void executeTask(Task& task);
    // 入口补充本地：从 incoming_ 取一批 push 进 deque[me]，返回是否取到
    bool fillLocalBatch(size_t me);
    // 从其他 worker 的 deque 偷一个任务，返回是否成功
    bool stealOne(Task& task, size_t me);
    // 所有 deque 与入口是否都空（近似，仅供唤醒/停止判定）
    bool allEmpty() const;

    std::vector<Worker> workers_;
    std::vector<std::unique_ptr<WorkStealingDeque<Task>>> deques_;
    LockFreeQueue<Task> incoming_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> active_submits_{0};
    std::atomic<uint64_t> submitted_tasks_{0};
    std::atomic<uint64_t> completed_tasks_{0};
    std::atomic<uint64_t> busy_workers_{0};
    std::atomic<uint64_t> failed_tasks_{0};
    mutable     std::mutex mtx_;
    std::condition_variable cv_;           // 空闲 worker 等待
    std::condition_variable not_full_cv_;  // BLOCK 生产者等待入口空间
    std::atomic<size_t> waiting_producers_{0};
    std::atomic<size_t> workers_idle_{0};  // 空闲（在 cv_ 等待）的 worker 数
    WorkStealingRejectPolicy policy_;
    size_t num_threads_;
    // 任务追踪（默认关闭）：先读 bool 再原子加载 sink，默认热路径只多一次分支。
    std::atomic<bool> trace_enabled_{false};
    std::shared_ptr<llengine::TaskTraceSink> trace_sink_;
};

template <typename F, typename... Args>
auto WorkStealingThreadPool::submit(F &&f, Args &&...args)
    -> std::future<typename std::invoke_result<F, Args...>::type>
{
    // RAII：标记在途提交，shutdown() 等待其归零，保证析构安全
    active_submits_.fetch_add(1, std::memory_order_acq_rel);
    struct SubmitGuard {
        std::atomic<uint32_t>& n;
        ~SubmitGuard() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{active_submits_};

    using return_type = typename std::invoke_result<F, Args...>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> res = task->get_future();

    // DISCARD 满时返回 false，忽略即可：Task 临时对象析构使 future 得 broken_promise
    enqueueTask(makeTraceableTask([task]() { (*task)(); }));
    return res;
}

template <typename F, typename... Args>
void WorkStealingThreadPool::submitVoid(F &&f, Args &&...args)
{
    active_submits_.fetch_add(1, std::memory_order_acq_rel);
    struct SubmitGuard {
        std::atomic<uint32_t>& n;
        ~SubmitGuard() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{active_submits_};

    WorkStealingThreadPool* self = this;
    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

    // 无 future：异常不能逃逸 worker，捕获并计入 failed_tasks。
    // 任务持有 this 安全：shutdown() 让 worker 排空所有 deque/入口后才 join。
    enqueueTask(makeTraceableTask([self, bound]() mutable {
        try {
            bound();
        } catch (...) {
            self->failed_tasks_.fetch_add(1, std::memory_order_relaxed);
        }
    }));
}

template <typename F, typename... Args>
bool WorkStealingThreadPool::trySubmit(F &&f, Args &&...args)
{
    active_submits_.fetch_add(1, std::memory_order_acq_rel);
    struct SubmitGuard {
        std::atomic<uint32_t>& n;
        ~SubmitGuard() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{active_submits_};

    if (stop_.load(std::memory_order_acquire)) {
        return false;
    }

    WorkStealingThreadPool* self = this;
    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

    return tryEnqueueTask(makeTraceableTask([self, bound]() mutable {
        try {
            bound();
        } catch (...) {
            self->failed_tasks_.fetch_add(1, std::memory_order_relaxed);
        }
    }));
}
