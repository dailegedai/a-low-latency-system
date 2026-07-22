#include "../include/ThreadPool.h"
#include <chrono>

ThreadPool::ThreadPool(size_t num_thread, size_t queue_size, RejectPolicy policy) : stop(false), max_queue_size(queue_size), reject_policy(policy)
{
    for (size_t i = 0; i < num_thread; i++) {
        workers.emplace_back(
            std::thread([this]()
                        {

                while(true) {
                    Task task([]{}); 

                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        cv.wait(lock, [this]() {
                            return stop || !tasks.empty();
                        });

                        if (stop && tasks.empty()) {
                            return;
                        }

                        task = std::move(tasks.front());
                        tasks.pop();

                        not_full_cv.notify_one();
                    }
                    
                    busy_workers.fetch_add(1);

                    task.execute();

                    busy_workers.fetch_sub(1);
                    completed_tasks.fetch_add(1);

                    // to do
                    // try
                    // {
                    //     task();
                    // }
                    // catch (...)
                    // {
                    //     busy_workers.fetch_sub(1);
                    //     completed_tasks.fetch_add(1);
                    //     throw;
                    // }
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
    return getBusyWorkerCount() == 0;
}

size_t ThreadPool::getThreadCount() const
{
    return workers.size();
}

bool ThreadPool::isStopping() const
{
    return stop;
}