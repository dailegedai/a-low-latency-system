#include "../include/ThreadPool.h"
#include <iostream>
#include <chrono>

int main() {
    ThreadPool pool(5);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100000; i++) {
        pool.submit([i]() {
            std::cout << "task" << i << std::endl;
        });
    }
    auto end = std::chrono::high_resolution_clock::now();

    std::cout << "Time: "
              << std::chrono::duration_cast<std::chrono::milliseconds>
                (end - start).count() 
              << "ms\n";


    std::this_thread::sleep_for(std::chrono::seconds(3));
}