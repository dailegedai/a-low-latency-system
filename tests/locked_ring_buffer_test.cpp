#include "../include/LockedRingBuffer.h"

#include "check.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

int main()
{
    std::cout << "========== Locked RingBuffer Test ==========\n";

    LockedRingBuffer<int> q(65536);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    constexpr int PRODUCERS = 4;
    constexpr int PER = 10000;
    constexpr int TOTAL = PRODUCERS * PER;

    for (int t = 0; t < PRODUCERS; ++t) {
        threads.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < PER; ++i) {
                q.push(std::move(i));
            }
        });
    }

    while (ready.load() < PRODUCERS) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    long got = 0;
    int x;
    while (got < TOTAL) {
        if (q.pop(x)) {
            ++got;
        }
    }

    CHECK(got == TOTAL);
    CHECK(q.empty());
    std::cout << "  consumed=" << got << " PASS\n";

    for (auto& th : threads) {
        th.join();
    }

    std::cout << "========== ALL TESTS PASSED ==========\n";
    return 0; 
}