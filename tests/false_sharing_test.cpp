#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

#include "check.h"

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
    const auto x_line =
        reinterpret_cast<std::uintptr_t>(&counter.x) & ~std::uintptr_t{63};
    const auto y_line =
        reinterpret_cast<std::uintptr_t>(&counter.y) & ~std::uintptr_t{63};
    CHECK(x_line != y_line);

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
    CHECK(counter.x.load() == 100000000);
    CHECK(counter.y.load() == 100000000);
    std::cout << "aligned counters: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
              << "ms PASS\n";
    return 0;
}
