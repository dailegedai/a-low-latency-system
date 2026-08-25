#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

// no false sharing: x/y 各自独立缓存行，cost ~200ms
struct Counter {
    alignas(64)
    std::atomic<long> x{0};
    alignas(64)
    std::atomic<long> y{0};
};

// false sharing: x/y 共享同一缓存行，cost ~1000ms
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