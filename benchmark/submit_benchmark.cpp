#include "../include/ThreadPool.h"
#include "benchmark_util.h"

#include <iostream>
#include <memory>

int main()
{
    constexpr int N = 1000000;

    // Each sample gets a fresh, empty, sufficiently large queue. Pool
    // construction is outside the timed region, so repeated samples do not
    // inherit workers, queued tasks, or BLOCK backpressure from prior samples.
    sample_with_setup(
        "submit 1M tasks",
        [N] {
            return std::make_unique<ThreadPool>(
                0, N, RejectPolicy::DISCARD);
        },
        [](std::unique_ptr<ThreadPool>& pool) {
            for (int i = 0; i < N; ++i) {
                pool->submit([] {});
            }
        },
        /*warmup=*/1,
        /*repeats=*/5);
}
