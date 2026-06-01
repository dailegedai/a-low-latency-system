#include "../include/ThreadPool.h"
#include <iostream>
#include <chrono>

int main() {
    // ThreadPool pool(5);

    // auto start = std::chrono::high_resolution_clock::now();
    // for (int i = 0; i < 100000; i++) {
    //     pool.submit([i]() {
    //         std::cout << "task" << i << std::endl;
    //     });
    // }
    // auto end = std::chrono::high_resolution_clock::now();

    // std::cout << "Time: "
    //           << std::chrono::duration_cast<std::chrono::milliseconds>
    //             (end - start).count() 
    //           << "ms\n";


    // std::this_thread::sleep_for(std::chrono::seconds(3));


    ThreadPool pool(4);

    auto future1 = pool.submit([](int a, int b) {
        return a + b;
    }, 1, 2);

    auto future2 = pool.submit([]() {
        return std::string("hello thread pool");
    });

    std::cout << future1.get() << std::endl;

    std::cout << future2.get() << std::endl;
}