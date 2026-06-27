#include "../include/ThreadPool.h"

#include <chrono>
#include <iostream>

int main()
{
    constexpr int N = 1000000;

    ThreadPool pool(4, N);

    auto start =
        std::chrono::steady_clock::now();

    for(int i = 0; i < N; ++i)
    {
        pool.submit([](){});
    }

    auto end =
        std::chrono::steady_clock::now();

    std::cout
        << "submit "
        << N
        << " tasks : "
        << std::chrono::duration_cast<
            std::chrono::milliseconds>(
                end-start).count()
        << " ms\n";
}