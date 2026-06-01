#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <queue>
#include <future>

class ThreadPool {
public:
    ThreadPool(size_t num_thread);
    ~ThreadPool();

    template<class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex mtx;
    std::condition_variable cv;
    bool stop{false};
};

template<class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type>
{
    using return_type = 
        typename std::invoke_result<F, Args...>::type;
    
    auto task = std::make_shared<
        std::packaged_task<return_type()>
    >(
        std::bind(
            std::forward<F>(f),
            std::forward<Args>(args)...
        )
    );

    std::future<return_type> res = task->get_future();

    {
        std::lock_guard<std::mutex> lock(mtx);

        if (stop) {
            throw std::runtime_error(
                "submit on stopped ThreadPool."
            );
        }

        tasks.emplace([task]() {
            (*task)();
        });
    }
    cv.notify_one();
    return res;
}


