#include "../include/ThreadPool.h"
#include  <iostream>
#include <chrono>

ThreadPool::ThreadPool(size_t num_thread, size_t queue_size, RejectPolicy policy) : stop(false), max_queue_size(queue_size), reject_policy(policy) {
    for (size_t i = 0; i < num_thread; i++) {
        workers.emplace_back([this]() {
            std::cout << "worker thread " 
            << std::this_thread::get_id() << "start \n";
            while(true) {
                std::function<void()> task;

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

                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        stop = true;
    }
    cv.notify_all();

    for (std::thread &worker : workers) {
        if (worker.joinable()) { 
            worker.join();
        }
    }
}
