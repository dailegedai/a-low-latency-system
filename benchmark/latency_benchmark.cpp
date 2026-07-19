#include "../include/ThreadPool.h"

#include "benchmark_util.h"

#include <cmath>
#include <numeric>
#include <vector>
#include <future>

const int N = 100000;

static double compute_pi(int terms)
{
    double sum = 0.0;
    for (int k = 0; k < terms; k++) {
        sum += (k % 2 == 0 ? 1.0 : -1.0) / (2 * k + 1);
    }
    return sum * 4.0;
}

int main()
{
    std::cout << "=== Latency Benchmark Suite ===\n\n";
    std::cout << "each test submits " << N << " tasks\n\n";

    // 1. Submit latency vs thread count
    {
        std::cout << "--- submit latency vs thread count ---\n";
        for (auto threads : {1, 2, 4, 8}) {
            ThreadPool pool(threads, 1024);
            {
                Benchmark bm(std::to_string(threads) + " threads, queue=1024");
                for (int i = 0; i < N; i++) {
                    pool.submit([] {});
                }
            }
            pool.shutdown();
        }
        std::cout << "\n";
    }

    // 2. Submit latency vs queue size
    {
        std::cout << "--- submit latency vs queue size (4 threads) ---\n";
        for (auto qsize : {4, 64, 1024, 65536}) {
            ThreadPool pool(4, qsize);
            {
                Benchmark bm("queue=" + std::to_string(qsize));
                for (int i = 0; i < N; i++) {
                    pool.submit([] {});
                }
            }
            pool.shutdown();
        }
        std::cout << "\n";
    }

    // 3. End-to-end latency (submit + execute + future.get())
    {
        std::cout << "--- end-to-end latency (submit + execute + future.get()) ---\n";
        ThreadPool pool(4, 1024);
        {
            Benchmark bm("e2e latency, void tasks");
            std::vector<std::future<void>> futs;
            futs.reserve(N);
            for (int i = 0; i < N; i++) {
                futs.push_back(pool.submit([] {}));
            }
            for (auto &f : futs) {
                f.get();
            }
        }
        pool.shutdown();
        std::cout << "\n";
    }

    // 4. End-to-end latency with return value
    {
        std::cout << "--- end-to-end latency (with return value) ---\n";
        ThreadPool pool(4, 1024);
        {
            Benchmark bm("e2e latency, int return");
            std::vector<std::future<int>> futs;
            futs.reserve(N);
            for (int i = 0; i < N; i++) {
                futs.push_back(pool.submit([] { return 42; }));
            }
            volatile int sum = 0;
            for (auto &f : futs) {
                sum += f.get();
            }
        }
        pool.shutdown();
        std::cout << "\n";
    }

    // 5. End-to-end latency with arguments
    {
        std::cout << "--- end-to-end latency (with arguments) ---\n";
        ThreadPool pool(4, 1024);
        {
            Benchmark bm("e2e latency, with args");
            std::vector<std::future<int>> futs;
            futs.reserve(N);
            for (int i = 0; i < N; i++) {
                futs.push_back(pool.submit([](int a, int b) { return a + b; }, i, i));
            }
            volatile int sum = 0;
            for (auto &f : futs) {
                sum += f.get();
            }
        }
        pool.shutdown();
        std::cout << "\n";
    }

    // 6. Task execution latency — CPU-bound work
    {
        std::cout << "--- execution latency (CPU-bound work) ---\n";
        ThreadPool pool(4, 1024);
        {
            Benchmark bm("CPU-bound pi(1000)");
            std::vector<std::future<double>> futs;
            futs.reserve(N / 10);
            for (int i = 0; i < N / 10; i++) {
                futs.push_back(pool.submit(compute_pi, 1000));
            }
            volatile double sum = 0.0;
            for (auto &f : futs) {
                sum += f.get();
            }
        }
        pool.shutdown();
        std::cout << "\n";
    }

    // 7. Queue full contention — BLOCK policy
    {
        std::cout << "--- queue full contention ---\n";
        {
            ThreadPool pool(2, 4, RejectPolicy::BLOCK);
            {
                Benchmark bm("BLOCK policy, queue=4, 2 threads");
                for (int i = 0; i < N; i++) {
                    pool.submit(compute_pi, 100);
                }
            }
            pool.shutdown();
        }
        {
            ThreadPool pool(2, 1024, RejectPolicy::THROW);
            int throw_count = 0;
            {
                Benchmark bm("THROW policy, queue=1024, 2 threads");
                for (int i = 0; i < N; i++) {
                    try {
                        pool.submit(compute_pi, 100);
                    } catch (const std::runtime_error &) {
                        throw_count++;
                    }
                }
            }
            std::cout << "  (THROW count: " << throw_count << ")\n";
            pool.shutdown();
        }
        std::cout << "\n";
    }

    // 8. Task with non-trivial function objects
    {
        std::cout << "--- non-trivial function objects ---\n";
        ThreadPool pool(4, 1024);
        {
            struct HeavyFunctor {
                std::vector<int> data;
                HeavyFunctor() : data(1024, 0) {}
                void operator()() const {}
            };
            Benchmark bm("heavy functor (copy 4KB)");
            for (int i = 0; i < N / 10; i++) {
                pool.submit(HeavyFunctor{});
            }
        }
        pool.shutdown();
        std::cout << "\n";
    }

    std::cout << "=== Done ===\n";
    return 0;
}
