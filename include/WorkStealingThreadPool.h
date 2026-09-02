#pragma once

#include "LockFreeQueue.h"
#include "Task.h"
#include "Worker.h"
#include "WorkStealingDeque.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
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

    void shutdown();

    uint64_t getSubmittedTaskCount() const;
    uint64_t getCompletedTaskCount() const;
    uint64_t getBusyWorkerCount() const;
    size_t getQueueSize();
    bool idle() const;
    size_t getThreadCount() const;
    bool isStopping() const;

private:
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
    mutable     std::mutex mtx_;
    std::condition_variable cv_;           // 空闲 worker 等待
    std::condition_variable not_full_cv_;  // BLOCK 生产者等待入口空间
    std::atomic<size_t> waiting_producers_{0};
    std::atomic<size_t> workers_idle_{0};  // 空闲（在 cv_ 等待）的 worker 数
    WorkStealingRejectPolicy policy_;
    size_t num_threads_;
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

    // 正常路径：无锁 push 到共享入口（LockFreeQueue 是 MPMC 无锁队列，
    // 持锁串行化是不必要的——这正是本池相对 ThreadPool 消除单锁的初衷）。
    // 仅当入口满、需要协调（BLOCK 等待 / THROW / DISCARD）时才取锁。
    Task t([task]() { (*task)(); });

    // 无锁路径：先检查 stop（原子读），避免 shutdown 后仍提交成功
    if (!stop_.load(std::memory_order_acquire) && incoming_.push(std::move(t))) {
        submitted_tasks_.fetch_add(1, std::memory_order_relaxed);
        // 有 worker 空闲则唤醒（低开销原子读判断，避免无效 notify）
        if (workers_idle_.load(std::memory_order_relaxed) > 0) {
            cv_.notify_one();
        }
        return res;
    }

    // 入口满 或 stop 已置位：进入策略处理（需要锁协调）
    {
        std::unique_lock<std::mutex> lock(mtx_);

        if (stop_.load(std::memory_order_acquire)) {
            throw std::runtime_error("submit on stopped WorkStealingThreadPool.");
        }

        if (policy_ == WorkStealingRejectPolicy::THROW) {
            throw std::runtime_error("task queue is full.");
        }
        if (policy_ == WorkStealingRejectPolicy::DISCARD) {
            return res; // 丢弃：future 得 broken_promise
        }

        // BLOCK：循环等待直到 push 成功。
        // 注意：full() 是 tail/head 异步快照，与 push 的判定可能不一致
        // （worker 无锁 pop 推进 head），因此不能依赖一次 wait 后必成功，
        // push 失败须继续等待。
        for (;;) {
            if (incoming_.push(std::move(t))) {
                break; // push 成功
            }
            waiting_producers_.fetch_add(1, std::memory_order_relaxed);
            not_full_cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_acquire) || !incoming_.full();
            });
            waiting_producers_.fetch_sub(1, std::memory_order_relaxed);
            if (stop_.load(std::memory_order_acquire)) {
                throw std::runtime_error("submit on stopped WorkStealingThreadPool.");
            }
        }
        submitted_tasks_.fetch_add(1, std::memory_order_relaxed);
    }
    if (workers_idle_.load(std::memory_order_relaxed) > 0) {
        cv_.notify_one(); // 唤醒一个空闲 worker
    }
    return res;
}
