#include "../include/ThreadPool.h"
#include <chrono>

ThreadPool::ThreadPool(size_t num_thread, size_t queue_size, RejectPolicy policy) 
    : tasks(queue_size),
    stop(false),
    reject_policy(policy)
{
    for (size_t i = 0; i < num_thread; i++) {
        workers.emplace_back(
            std::thread([this]()
                        {

                while(true) {
                    Task task; 
                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        cv.wait(lock, [this]() {
                            return stop || !tasks.empty();
                        });

                        if (stop && tasks.empty()) {
                            return;
                        }

                        if (!tasks.pop(task)) {
                            continue;
                        }
                        not_full_cv.notify_one();
                    }
                    
                    busy_workers.fetch_add(1);

                    task.execute();

                    busy_workers.fetch_sub(1);
                    completed_tasks.fetch_add(1);

                    // 无需 try/catch：用户任务异常由 std::packaged_task 内部捕获并存
                    // 入共享状态，future.get() 时再抛出，不会逃逸到 worker 线程体。
                }
            }
        )
    );}
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (stop) {
            return;
        }
        stop = true;
    }

    cv.notify_all();
    // 唤醒所有阻塞在 BLOCK 策略下的生产者，使其在 stop 谓词下放行退出
    not_full_cv.notify_all();

    for (Worker &worker : workers) {
        worker.join();
    }
}

uint64_t ThreadPool::getSubmittedTaskCount() const
{
    return submitted_tasks.load();
}

uint64_t ThreadPool::getCompletedTaskCount() const
{
    return completed_tasks.load();
}

uint64_t ThreadPool::getBusyWorkerCount() const
{
    return busy_workers.load();
}

uint64_t ThreadPool::getQueueSize()
{
    std::lock_guard<std::mutex> lock(mtx);
    return tasks.size();
}

bool ThreadPool::idle() const
{
    // 空闲 = 无 worker 在执行 且 队列为空（避免 worker 恰好执行完、队列仍有待办时的误判）
    std::lock_guard<std::mutex> lock(mtx);
    return busy_workers.load() == 0 && tasks.empty();
}

size_t ThreadPool::getThreadCount() const
{
    return workers.size();
}

bool ThreadPool::isStopping() const
{
    return stop;
}