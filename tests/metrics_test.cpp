#include "../include/ThreadPool.h"

#include <chrono>
#include <iostream>

int main() {
    std::cout << "========== Metrics Test ==========\n";
    ThreadPool pool(4, 100);

    for (int i = 0; i < 20; ++i) {
        pool.submit([](){
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500)
            );
        });
    }

    while (pool.getCompletedTaskCount() < 20) {
        std::cout << "submitted: " << pool.getSubmittedTaskCount() << "\n";
        std::cout << "completed: " << pool.getCompletedTaskCount() << "\n";
        std::cout << "busy: " << pool.getBusyWorkerCount() << "\n";
        std::cout << "queue: " << pool.getQueueSize() << "\n";

        std::this_thread::sleep_for(
            std::chrono::milliseconds(200)
        );
    }

    std::cout << "all tasks completed\n";
    std::cout << "========== Metrics Test PASSED ==========\n";
}