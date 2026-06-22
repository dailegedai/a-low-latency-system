#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <queue>
#include <future>

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

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::condition_variable not_full_cv;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop{false};
    size_t max_queue_size;
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
                    return tasks.size() < max_queue_size;
                });
            break;
        }

            // case RejectPolicy::DISCARD:
            // {
            //     if (tasks.size() >= max_queue_size)
            //     {
            //         return std::futrue<return_type>{};
            /*
            future.get()会出问题。因为：future没有关联shared state
            属于无效 future。
            */
            //     }
            //     break;
            // }

        case RejectPolicy::THROW:
        {
            if (tasks.size() >= max_queue_size)
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

        tasks.emplace([task]()
                      { (*task)(); });
    }
    cv.notify_one();
    return res;
}
