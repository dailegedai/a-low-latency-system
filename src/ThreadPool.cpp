#include "../include/ThreadPool.h"
#include  <iostream>

ThreadPool::ThreadPool(size_t num_thread) : stop(false) {
    for (size_t i = 0; i < num_thread; i++) {
        workers.emplace_back([this]() {
            std::cout << "worker thread " 
            << std::this_thread::get_id() << "start \n";
            while(true) {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(this->mtx);

                    std::cout << "worker waiting ...\n";
                    this->cv.wait(lock, [this]() {
                        return this->stop || !this->tasks.empty();
                    });

                    if (this->stop && this->tasks.empty()) {
                        return;
                    }

                    task = this->tasks.front();
                    this->tasks.pop();
                }

                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(this->mtx);
        this->stop = true;
    }
    this->cv.notify_all();

    for (std::thread &worker : this->workers) {
        worker.join();
    }
}
void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(this->mtx);
        this->tasks.push(move(task));
    }
    cv.notify_one();
}

int main() {
    ThreadPool pool(5);
    for (int i = 0; i < 10; i++) {
        pool.submit([i]() {
            std::cout << "task" << i << std::endl;
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
}