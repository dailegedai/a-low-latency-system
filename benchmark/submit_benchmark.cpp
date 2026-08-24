#include "../include/ThreadPool.h"
#include "benchmark_util.h"

#include <iostream>

int main()
{
    constexpr int N = 1000000;

    ThreadPool pool(4, N);

    sample("submit 1M tasks", [&] {
        for (int i = 0; i < N; ++i) {
            pool.submit([](){});
        }
    }, /*warmup=*/1, /*repeats=*/5);
}