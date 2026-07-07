#include "../include/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

// true sharing cost 200~ms
struct Counter {
    alignas(64)
    std::atomic<long> x{0};
    alignas(64)
    std::atomic<long> y{0};
};

// false sharing cost 1000~ms
// struct Counter {
//     std::atomic<long> x{0};
//     std::atomic<long> y{0};
// };

Counter counter;

int main() {
    auto start = std::chrono::steady_clock::now();

    std::thread t1([](){ 
        for (long i = 0; i < 100000000; i++) {
            counter.x++;
        }
    });

    std::thread t2([](){ 
        for (long i = 0; i < 100000000; i++) {
            counter.y++;
        }
    });

    t1.join();
    t2.join();

    auto end = std::chrono::steady_clock::now();
    std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms\n";
}