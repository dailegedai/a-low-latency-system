#pragma once
#include "Task.h"
#include "Worker.h"
#include "RingBuffer.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <future>
#include <functional>
#include <utility>
#include <atomic>

// x86-64 L1 cache line（probe 已确认 == std::hardware_destructive_interference_size）
static constexpr std::size_t kCacheLineSize = 64;

enum class RejectPolicy
{
    BLOCK,
    DISCARD,
    THROW
};

class ThreadPool
{
public:
    ThreadPool(size_t num_thread, size_t queue_size, RejectPolicy policy = RejectPolicy::BLOCK);
    ~ThreadPool();

    // 生命周期契约：
    // - submit() 可在任意线程并发调用；
    // - shutdown() 会等待所有在途 submit() 完成后再返回（通过 active_submits 计数），
    //   因此析构函数体内的 shutdown() 能保证成员（mtx/tasks/cv）销毁时无在途提交；
    // - 调用方仍须保证 shutdown() 之后不再发起新的 submit()（并发调 submit 到已销毁对象属调用方缺陷）。
    template <class F, class... Args>
    auto submit(F &&f, Args &&...args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    // 无 future 的 fire-and-forget 提交：省去 packaged_task/shared_ptr 分配。
    // 遵循池的 RejectPolicy；任务异常由 worker 侧捕获并计入 getFailedTaskCount()。
    template <class F, class... Args>
    void submitVoid(F &&f, Args &&...args);

    // 非阻塞 fire-and-forget：队列满或已停止返回 false，不抛异常。
    template <class F, class... Args>
    bool trySubmit(F &&f, Args &&...args);

    void shutdown();

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
    // 非阻塞入队：队列满或 stop 返回 false（不抛异常）。
    bool tryEnqueueTask(Task &&task);

    std::vector<Worker> workers;
    RingBuffer<Task> tasks;
    std::condition_variable not_full_cv;
    alignas(kCacheLineSize) std::atomic<uint64_t> submitted_tasks{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> completed_tasks{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> busy_workers{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> failed_tasks{0};
    mutable std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    RejectPolicy reject_policy;
    // 在途 submit 计数：shutdown() 等待其归零，确保析构安全
    std::atomic<uint32_t> active_submits{0};
};

template <typename F, typename... Args>
auto ThreadPool::submit(F &&f, Args &&...args)
    -> std::future<typename std::invoke_result<F, Args...>::type>
{
    // RAII：标记在途提交，shutdown() 会等待本计数归零后才销毁成员
    active_submits.fetch_add(1, std::memory_order_acq_rel);
    struct SubmitGuard {
        std::atomic<uint32_t>& n;
        ~SubmitGuard() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{active_submits};

    using return_type =
        typename std::invoke_result<F, Args...>::type;

    auto task = std::make_shared<
        std::packaged_task<return_type()>>(
        std::bind(
            std::forward<F>(f),
            std::forward<Args>(args)...));

    std::future<return_type> res = task->get_future();

    // DISCARD 满时返回 false，忽略即可：Task 临时对象析构释放 shared_ptr，
    // 使 future 得到 broken_promise（与旧行为一致）。
    enqueueTask(Task([task]() { (*task)(); }));
    return res;
}

template <typename F, typename... Args>
void ThreadPool::submitVoid(F &&f, Args &&...args)
{
    // RAII：与 submit 同一生命周期契约（shutdown 等待在途提交）
    active_submits.fetch_add(1, std::memory_order_acq_rel);
    struct SubmitGuard {
        std::atomic<uint32_t>& n;
        ~SubmitGuard() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{active_submits};

    ThreadPool* self = this;
    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

    // 无 future：异常不能逃逸 worker，故在包装层捕获并计入 failed_tasks。
    // 任务持有 this 是安全的：shutdown() 会让 worker 排空队列后才 join。
    enqueueTask(Task([self, bound]() mutable {
        try {
            bound();
        } catch (...) {
            self->failed_tasks.fetch_add(1, std::memory_order_relaxed);
        }
    }));
}

template <typename F, typename... Args>
bool ThreadPool::trySubmit(F &&f, Args &&...args)
{
    active_submits.fetch_add(1, std::memory_order_acq_rel);
    struct SubmitGuard {
        std::atomic<uint32_t>& n;
        ~SubmitGuard() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{active_submits};

    // 先读 stop 快速拒绝：避免停用后仍做 std::bind / 构造 Task 的分配
    if (stop.load(std::memory_order_acquire)) {
        return false;
    }

    ThreadPool* self = this;
    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

    return tryEnqueueTask(Task([self, bound]() mutable {
        try {
            bound();
        } catch (...) {
            self->failed_tasks.fetch_add(1, std::memory_order_relaxed);
        }
    }));
}
