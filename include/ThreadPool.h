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
    size_t max_queue_size;
    std::vector<Worker> workers;
    RingBuffer<Task> tasks;
    std::condition_variable not_full_cv;
    alignas(kCacheLineSize) std::atomic<uint64_t> submitted_tasks{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> completed_tasks{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> busy_workers{0};
    std::mutex mtx;
    std::condition_variable cv;
    bool stop{false};
    RejectPolicy reject_policy;
};

template <typename F, typename... Args>
auto ThreadPool::submit(F &&f, Args &&...args)
    -> std::future<typename std::invoke_result<F, Args...>::type>
{
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
                    return !tasks.full();
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
