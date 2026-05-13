#include "../include/ThreadPool.h"

#include <chrono>
#include <iostream>

int main() {

    ThreadPool pool(4);

    auto start =
        std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 100000; ++i) {
        pool.submit([](){});
    }

    auto end =
        std::chrono::high_resolution_clock::now();

    auto duration =
        std::chrono::duration_cast<
            std::chrono::milliseconds
        >(end - start);

    std::cout << "submit time: "
              << duration.count()
              << " ms\n";
}