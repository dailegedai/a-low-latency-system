#pragma once
#include "Task.h"
#include "Worker.h"
#include "RingBuffer.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <future>
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

    void shutdown();

    uint64_t getSubmittedTaskCount() const;
    uint64_t getCompletedTaskCount() const;
    uint64_t getBusyWorkerCount() const;
    size_t getQueueSize();
    bool idle() const;
    size_t getThreadCount() const;
    bool isStopping() const;

private:
    std::vector<Worker> workers;
    RingBuffer<Task> tasks;
    std::condition_variable not_full_cv;
    alignas(kCacheLineSize) std::atomic<uint64_t> submitted_tasks{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> completed_tasks{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> busy_workers{0};
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

    {
        std::unique_lock<std::mutex> lock(mtx);

        switch (reject_policy)
        {
        case RejectPolicy::BLOCK:
        {
            not_full_cv.wait(
                lock,
                [this]
                {
                    // stop 时放行，使阻塞的生产者在 shutdown 后能退出（见 shutdown 的 not_full_cv.notify_all）
                    return stop || !tasks.full();
                });
            break;
        }

        case RejectPolicy::DISCARD:
        {
            if (tasks.full())
            {
                return res;
            }
            break;
        }

        case RejectPolicy::THROW:
        {
            if (tasks.full())
            {
                throw std::runtime_error("task queue is full.");
            }
            break;
        }
        }

        if (stop)
        {
            throw std::runtime_error(
                "submit on stopped ThreadPool.");
        }

        tasks.push(
            Task(
                [task]() { 
                    (*task)(); 
                }
            )
        );
        submitted_tasks.fetch_add(1);              
    }
    cv.notify_one();
    return res;
}
