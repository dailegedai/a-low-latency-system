#include "../include/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

uint64_t now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

uint64_t percentile(const std::vector<uint64_t>& sorted, double p)
{
    if (sorted.empty()) {
        return 0;
    }
    const double index = p * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(index);
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double fraction = index - static_cast<double>(lo);
    return static_cast<uint64_t>(
        static_cast<double>(sorted[lo]) +
        fraction * static_cast<double>(sorted[hi] - sorted[lo]));
}

double compute_pi(int terms)
{
    double sum = 0.0;
    for (int k = 0; k < terms; ++k) {
        sum += (k % 2 == 0 ? 1.0 : -1.0) / (2 * k + 1);
    }
    return sum * 4.0;
}

void wait_for_count(std::atomic<int>& done, int expected)
{
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (done.load(std::memory_order_acquire) < expected) {
        if (Clock::now() >= deadline) {
            throw std::runtime_error("latency benchmark task wait timed out");
        }
        std::this_thread::yield();
    }
}

void warm_pool(ThreadPool& pool, int tasks)
{
    std::atomic<int> done{0};
    for (int i = 0; i < tasks; ++i) {
        pool.submitVoid([&done] {
            done.fetch_add(1, std::memory_order_release);
        });
    }
    wait_for_count(done, tasks);
}

void print_latency(
    const char* label,
    int threads,
    size_t queue_size,
    std::vector<uint64_t> samples,
    uint64_t elapsed_ns)
{
    std::sort(samples.begin(), samples.end());
    uint64_t sum = 0;
    for (uint64_t value : samples) {
        sum += value;
    }
    const double mean = static_cast<double>(sum) / samples.size();
    const double throughput =
        samples.size() * 1e9 / static_cast<double>(elapsed_ns);
    std::cout << label << " [threads=" << threads
              << " queue=" << queue_size << " samples=" << samples.size()
              << " throughput=" << throughput << " task/s]\n"
              << "  mean=" << mean
              << " p50=" << percentile(samples, 0.50)
              << " p90=" << percentile(samples, 0.90)
              << " p99=" << percentile(samples, 0.99)
              << " p999=" << percentile(samples, 0.999)
              << " max=" << samples.back() << " ns\n";
}

void bench_submit_void(
    const char* label,
    int threads,
    size_t queue_size,
    int tasks,
    int work_terms)
{
    ThreadPool pool(threads, queue_size);
    warm_pool(pool, std::min(tasks, 2000));

    std::vector<uint64_t> starts(static_cast<size_t>(tasks));
    std::vector<uint64_t> latencies(static_cast<size_t>(tasks));
    std::atomic<int> done{0};
    const uint64_t batch_start = now_ns();

    for (int i = 0; i < tasks; ++i) {
        starts[static_cast<size_t>(i)] = now_ns();
        pool.submitVoid([&, i] {
            if (work_terms != 0) {
                volatile double result = compute_pi(work_terms);
                (void)result;
            }
            latencies[static_cast<size_t>(i)] =
                now_ns() - starts[static_cast<size_t>(i)];
            done.fetch_add(1, std::memory_order_release);
        });
    }

    wait_for_count(done, tasks);
    const uint64_t elapsed_ns = now_ns() - batch_start;
    pool.shutdown();
    print_latency(
        label, threads, queue_size, std::move(latencies), elapsed_ns);
}

void bench_future_latency(int threads, size_t queue_size, int tasks)
{
    ThreadPool pool(threads, queue_size);
    warm_pool(pool, std::min(tasks, 1000));

    std::vector<uint64_t> starts(static_cast<size_t>(tasks));
    std::vector<uint64_t> latencies(static_cast<size_t>(tasks));
    std::vector<std::future<int>> futures;
    futures.reserve(static_cast<size_t>(tasks));
    const uint64_t batch_start = now_ns();

    for (int i = 0; i < tasks; ++i) {
        starts[static_cast<size_t>(i)] = now_ns();
        futures.push_back(pool.submit([&, i] {
            latencies[static_cast<size_t>(i)] =
                now_ns() - starts[static_cast<size_t>(i)];
            return i;
        }));
    }
    for (auto& future : futures) {
        if (future.get() < 0) {
            throw std::runtime_error("unexpected future result");
        }
    }

    const uint64_t elapsed_ns = now_ns() - batch_start;
    pool.shutdown();
    print_latency(
        "submit(future)", threads, queue_size, std::move(latencies), elapsed_ns);
}

} // namespace

int main()
{
    std::cout << "=== Task latency benchmark (submit -> completion) ===\n\n";

    constexpr int TASKS = 100000;
    for (int threads : {1, 2, 4, 8}) {
        bench_submit_void(
            "submitVoid empty", threads, 1024, TASKS, 0);
    }

    for (size_t queue_size : {4u, 64u, 1024u, 65536u}) {
        bench_submit_void(
            "submitVoid queue-size", 4, queue_size, TASKS, 0);
    }

    bench_submit_void(
        "submitVoid pi(1000)", 4, 1024, TASKS / 10, 1000);
    bench_future_latency(4, 1024, TASKS / 10);
    return 0;
}
