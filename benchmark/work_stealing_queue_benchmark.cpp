#include "../include/WorkStealingDeque.h"
#include "../include/WorkStealingQueue.h"
#include "benchmark_environment.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

// V1: fixed-capacity WorkStealingDeque with an atomic bottom.
// V2: QueueWorker/QueueStealer with an owner-private bottom and epoch-based grow.
//
// This benchmark separates setup from the timed region:
//   create queues + threads -> wait until ready -> start timer -> push/steal
//   -> all workers signal completion -> stop timer -> join.
//
// V1/V2 order alternates by default so drift in host scheduling cannot
// systematically favor whichever implementation happens to run second.

namespace {

constexpr int kOwners = 4;
constexpr int kThieves = 4;
constexpr int kPerOwner = 100000;
constexpr long kTotal = kOwners * kPerOwner;
constexpr int kScenario1Threads = kOwners;
constexpr int kScenario2Threads = kOwners + kThieves;
constexpr size_t kStealBatchSize = 16;

using llengine::QueueStealer;
using llengine::QueueWorker;
using Clock = std::chrono::steady_clock;

enum class Version
{
    V1,
    V2
};

enum class Order
{
    Alternate,
    AlternateV2,
    V1First,
    V2First
};

enum class AllocationMode
{
    System,
    Arena
};

class BumpArena;

struct SystemValue {
    explicit SystemValue(long v = 0) : value(v) {}

    static constexpr bool kUsesArena = false;
    static void set_arena(BumpArena*) {}

    long value;
};

class BumpArena {
public:
    explicit BumpArena(size_t bytes)
        : storage_(std::make_unique<std::byte[]>(bytes)),
          capacity_(bytes)
    {
        // Pay page-fault/cold-cache costs outside the timed region.
        std::fill(storage_.get(), storage_.get() + capacity_, std::byte{0});
    }

    void* allocate(size_t size, size_t alignment)
    {
        const auto base = reinterpret_cast<uintptr_t>(storage_.get());
        const uintptr_t current = base + used_;
        const uintptr_t aligned =
            (current + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
        const size_t offset = static_cast<size_t>(aligned - base);
        if (offset > capacity_ || size > capacity_ - offset) {
            throw std::bad_alloc();
        }
        used_ = offset + size;
        ++allocations_;
        return reinterpret_cast<void*>(aligned);
    }

    uint64_t allocations() const
    {
        return allocations_;
    }

private:
    std::unique_ptr<std::byte[]> storage_;
    size_t capacity_;
    size_t used_ = 0;
    uint64_t allocations_ = 0;
};

struct ArenaValue {
    explicit ArenaValue(long v = 0) : value(v) {}

    static constexpr bool kUsesArena = true;

    static void set_arena(BumpArena* arena)
    {
        current_arena_ = arena;
    }

    static void* operator new(size_t size)
    {
        if (current_arena_ == nullptr) {
            std::abort();
        }
        return current_arena_->allocate(size, alignof(ArenaValue));
    }

    static void operator delete(void*) noexcept {}
    static void operator delete(void*, size_t) noexcept {}

    static inline thread_local BumpArena* current_arena_ = nullptr;

    long value;
};

template <typename Value>
class AllocationContext {
public:
    explicit AllocationContext(size_t values_per_owner)
    {
        if constexpr (Value::kUsesArena) {
            const size_t bytes_per_owner =
                (values_per_owner + 64) *
                (sizeof(Value) + alignof(std::max_align_t));
            arenas_.reserve(kOwners);
            for (int i = 0; i < kOwners; ++i) {
                arenas_.push_back(std::make_unique<BumpArena>(bytes_per_owner));
            }
        }
    }

    void bind_current_thread(int worker_id)
    {
        if constexpr (Value::kUsesArena) {
            BumpArena* arena =
                worker_id >= 0 && worker_id < kOwners
                    ? arenas_[static_cast<size_t>(worker_id)].get()
                    : nullptr;
            Value::set_arena(arena);
        }
    }

    uint64_t allocations() const
    {
        uint64_t total = 0;
        for (const auto& arena : arenas_) {
            total += arena->allocations();
        }
        return total;
    }

private:
    std::vector<std::unique_ptr<BumpArena>> arenas_;
};

struct RunResult {
    double elapsed_ms = 0.0;
    int pinned_threads = 0;
    uint64_t arena_allocations = 0;
};

struct Summary {
    double mean = 0.0;
    double stddev = 0.0;
    double cv_percent = 0.0;
    double median = 0.0;
    double p90 = 0.0;
    double p99 = 0.0;
    double min = 0.0;
    double max = 0.0;
};

class ParallelRunState {
public:
    explicit ParallelRunState(int participants) : participants_(participants) {}

    void worker_ready()
    {
        ready_.fetch_add(1, std::memory_order_release);
    }

    void wait_for_workers()
    {
        while (ready_.load(std::memory_order_acquire) < participants_) {
            pause_or_yield();
        }
    }

    void start()
    {
        start_.store(true, std::memory_order_release);
    }

    void wait_for_start()
    {
        while (!start_.load(std::memory_order_acquire)) {
            pause_or_yield();
        }
    }

    void worker_done()
    {
        const auto completed_at = Clock::now();
        const int completed = finished_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (completed == participants_) {
            {
                std::lock_guard<std::mutex> lock(done_mutex_);
                end_time_ = completed_at;
                done_ = true;
            }
            done_cv_.notify_one();
        }
    }

    void wait_for_completion()
    {
        std::unique_lock<std::mutex> lock(done_mutex_);
        done_cv_.wait(lock, [this] { return done_; });
    }

    Clock::time_point end_time() const
    {
        std::lock_guard<std::mutex> lock(done_mutex_);
        return end_time_;
    }

    static void pause_or_yield()
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }

private:
    int participants_;
    std::atomic<int> ready_{0};
    std::atomic<bool> start_{false};
    std::atomic<int> finished_{0};
    mutable std::mutex done_mutex_;
    std::condition_variable done_cv_;
    bool done_ = false;
    Clock::time_point end_time_{};
};

std::vector<int> select_cpus(const std::vector<int>& allowed, int count)
{
    std::vector<int> selected;
    if (allowed.empty()) {
        return selected;
    }
    selected.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        selected.push_back(allowed[static_cast<size_t>(i) % allowed.size()]);
    }
    return selected;
}

template <typename MakeWorker>
RunResult timed_workers(
    int participants, const std::vector<int>& cpus, MakeWorker make_worker)
{
    ParallelRunState state(participants);
    std::atomic<int> pinned{0};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(participants));

    for (int id = 0; id < participants; ++id) {
        threads.emplace_back([&, id] {
            if (!cpus.empty() && pin_current_thread(cpus[static_cast<size_t>(id)])) {
                pinned.fetch_add(1, std::memory_order_relaxed);
            }

            // Construct worker-local views before the timed region.
            auto work = make_worker(id);
            state.worker_ready();
            state.wait_for_start();
            work();
            state.worker_done();
        });
    }

    // Thread creation is deliberately outside the measured interval.
    state.wait_for_workers();
    const auto start = Clock::now();
    state.start();
    state.wait_for_completion();
    const auto end = state.end_time();

    for (auto& thread : threads) {
        thread.join();
    }

    RunResult result;
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.pinned_threads =
        pinned.load(std::memory_order_relaxed);
    return result;
}

template <typename Value>
RunResult run_scenario1(
    Version version, const std::vector<int>& cpus)
{
    AllocationContext<Value> allocations(kPerOwner);
    if (version == Version::V1) {
        std::vector<std::unique_ptr<WorkStealingDeque<Value>>> deques;
        deques.reserve(kOwners);
        for (int i = 0; i < kOwners; ++i) {
            deques.push_back(
                std::make_unique<WorkStealingDeque<Value>>(kPerOwner + 64));
        }
        RunResult result = timed_workers(kScenario1Threads, cpus, [&](int id) {
            allocations.bind_current_thread(id);
            return [&deques, id] {
                for (int i = 0; i < kPerOwner; ++i) {
                    const Value value(
                        static_cast<long>(id) * kPerOwner + i);
                    while (!deques[static_cast<size_t>(id)]->push(value)) {
                        std::this_thread::yield();
                    }
                }
            };
        });
        result.arena_allocations = allocations.allocations();
        return result;
    }

    std::vector<std::unique_ptr<QueueWorker<Value>>> workers;
    workers.reserve(kOwners);
    for (int i = 0; i < kOwners; ++i) {
        workers.push_back(std::make_unique<QueueWorker<Value>>(kPerOwner + 64));
    }
    RunResult result = timed_workers(kScenario1Threads, cpus, [&](int id) {
        allocations.bind_current_thread(id);
        return [&workers, id] {
            for (int i = 0; i < kPerOwner; ++i) {
                workers[static_cast<size_t>(id)]->push(
                    Value(static_cast<long>(id) * kPerOwner + i));
            }
        };
    });
    result.arena_allocations = allocations.allocations();
    return result;
}

template <typename Value>
RunResult run_scenario2(
    Version version, const std::vector<int>& cpus)
{
    AllocationContext<Value> allocations(kPerOwner);
    std::atomic<long> popped{0};

    if (version == Version::V1) {
        std::vector<std::unique_ptr<WorkStealingDeque<Value>>> deques;
        deques.reserve(kOwners);
        for (int i = 0; i < kOwners; ++i) {
            deques.push_back(
                std::make_unique<WorkStealingDeque<Value>>(kPerOwner + 64));
        }
        RunResult result = timed_workers(kScenario2Threads, cpus, [&](int id) {
            allocations.bind_current_thread(id);
            if (id < kOwners) {
                return std::function<void()>([&deques, id] {
                    for (int i = 0; i < kPerOwner; ++i) {
                        const Value value(
                            static_cast<long>(id) * kPerOwner + i);
                        while (!deques[static_cast<size_t>(id)]->push(value)) {
                            std::this_thread::yield();
                        }
                    }
                });
            }
            return std::function<void()>([&deques, &popped] {
                Value values[kStealBatchSize];
                while (popped.load(std::memory_order_relaxed) < kTotal) {
                    for (auto& deque : deques) {
                        size_t got = 0;
                        while (got < kStealBatchSize) {
                            if (!deque->steal(values[got])) {
                                break;
                            }
                            ++got;
                        }
                        if (got != 0) {
                            popped.fetch_add(
                                static_cast<long>(got), std::memory_order_relaxed);
                            break;
                        }
                    }
                    std::this_thread::yield();
                }
            });
        });
        result.arena_allocations = allocations.allocations();
        return result;
    }

    std::vector<std::unique_ptr<QueueWorker<Value>>> workers;
    workers.reserve(kOwners);
    for (int i = 0; i < kOwners; ++i) {
        workers.push_back(std::make_unique<QueueWorker<Value>>(kPerOwner + 64));
    }
    RunResult result = timed_workers(kScenario2Threads, cpus, [&](int id) {
        allocations.bind_current_thread(id);
        if (id < kOwners) {
            return std::function<void()>([&workers, id] {
                for (int i = 0; i < kPerOwner; ++i) {
                    workers[static_cast<size_t>(id)]->push(
                        Value(static_cast<long>(id) * kPerOwner + i));
                }
            });
        }

        std::vector<QueueStealer<Value>> stealers;
        stealers.reserve(workers.size());
        for (auto& worker : workers) {
            stealers.push_back(worker->make_stealer());
        }
        return std::function<void()>(
            [&popped, stealers = std::move(stealers)]() mutable {
                Value values[kStealBatchSize];
                while (popped.load(std::memory_order_relaxed) < kTotal) {
                    for (auto& stealer : stealers) {
                        const size_t got =
                            stealer.stealBatch(values, kStealBatchSize);
                        if (got != 0) {
                            popped.fetch_add(
                                static_cast<long>(got), std::memory_order_relaxed);
                            break;
                        }
                    }
                    std::this_thread::yield();
                }
            });
    });
    result.arena_allocations = allocations.allocations();
    return result;
}

double percentile(const std::vector<double>& sorted, double p)
{
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double index =
        p * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(index);
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double fraction = index - static_cast<double>(lo);
    return sorted[lo] + fraction * (sorted[hi] - sorted[lo]);
}

Summary summarize(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const double sum =
        std::accumulate(values.begin(), values.end(), 0.0);
    const double mean = sum / static_cast<double>(values.size());
    double squared_difference = 0.0;
    for (double value : values) {
        const double diff = value - mean;
        squared_difference += diff * diff;
    }
    const double stddev =
        values.size() > 1
            ? std::sqrt(squared_difference / static_cast<double>(values.size() - 1))
            : 0.0;

    Summary summary;
    summary.mean = mean;
    summary.stddev = stddev;
    summary.cv_percent = mean != 0.0 ? 100.0 * stddev / mean : 0.0;
    summary.median = percentile(values, 0.50);
    summary.p90 = percentile(values, 0.90);
    summary.p99 = percentile(values, 0.99);
    summary.min = values.front();
    summary.max = values.back();
    return summary;
}

void print_summary(const char* label, const std::vector<double>& values)
{
    const Summary summary = summarize(values);
    std::cout << label << " n=" << values.size()
              << " mean=" << summary.mean
              << "ms sd=" << summary.stddev
              << "ms cv=" << summary.cv_percent << "%"
              << " median=" << summary.median
              << "ms p90=" << summary.p90
              << "ms p99=" << summary.p99
              << "ms min=" << summary.min
              << "ms max=" << summary.max << "ms\n";
}

const char* order_name(Order order)
{
    switch (order) {
    case Order::V1First:
        return "v1-first";
    case Order::V2First:
        return "v2-first";
    case Order::Alternate:
        return "alternate";
    case Order::AlternateV2:
        return "alternate-v2";
    }
    return "unknown";
}

const char* allocation_mode_name(AllocationMode mode)
{
    return mode == AllocationMode::Arena ? "arena" : "system";
}

bool v1_goes_first(Order order, int round)
{
    if (order == Order::V1First) {
        return true;
    }
    if (order == Order::V2First) {
        return false;
    }
    if (order == Order::AlternateV2) {
        return (round % 2) != 0;
    }
    return (round % 2) == 0;
}

void record_pair(
    int scenario,
    int round,
    bool v1_first,
    const RunResult& v1,
    const RunResult& v2,
    std::vector<double>& v1_times,
    std::vector<double>& v2_times)
{
    v1_times.push_back(v1.elapsed_ms);
    v2_times.push_back(v2.elapsed_ms);
    std::cout << "RESULT," << scenario << ',' << round << ','
              << (v1_first ? "v1-first" : "v2-first") << ','
              << std::fixed << std::setprecision(6)
              << v1.elapsed_ms << ',' << v2.elapsed_ms << ','
              << v1.arena_allocations << ',' << v2.arena_allocations << "\n";
}

void print_scenario_summary(
    int scenario,
    int first_pinned,
    int second_pinned,
    const std::vector<double>& v1_times,
    const std::vector<double>& v2_times)
{
    std::cout << "\n=== Scenario " << scenario << " ===" << "\n";
    std::cout << "pinned_threads: V1=" << first_pinned
              << '/' << (scenario == 1 ? kScenario1Threads : kScenario2Threads)
              << " V2=" << second_pinned
              << '/' << (scenario == 1 ? kScenario1Threads : kScenario2Threads)
              << "\n";
    print_summary("V1", v1_times);
    print_summary("V2", v2_times);

    std::vector<double> speedup;
    speedup.reserve(v1_times.size());
    for (size_t i = 0; i < v1_times.size(); ++i) {
        speedup.push_back(v1_times[i] / v2_times[i]);
    }
    const Summary summary = summarize(speedup);
    size_t v2_wins = 0;
    for (double value : speedup) {
        if (value > 1.0) {
            ++v2_wins;
        }
    }
    std::cout << "V2 speedup (V1/V2) n=" << speedup.size()
              << " mean=" << summary.mean
              << "x median=" << summary.median
              << "x min=" << summary.min
              << "x max=" << summary.max
              << "x V2_faster=" << v2_wins << '/' << speedup.size() << "\n";
}

bool parse_positive_int(const std::string& text, int& value)
{
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed <= 0 || parsed > 100000) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

int pinned_min(int current, int candidate)
{
    return current == -1 ? candidate : std::min(current, candidate);
}

template <typename Value>
int run_benchmark(
    int rounds,
    Order order,
    const std::vector<int>& scenario1_cpus,
    const std::vector<int>& scenario2_cpus)
{
    std::cout << "\n=== warmup ===\n";
    run_scenario1<Value>(Version::V1, scenario1_cpus);
    run_scenario1<Value>(Version::V2, scenario1_cpus);
    run_scenario2<Value>(Version::V1, scenario2_cpus);
    run_scenario2<Value>(Version::V2, scenario2_cpus);

    std::vector<double> scenario1_v1;
    std::vector<double> scenario1_v2;
    std::vector<double> scenario2_v1;
    std::vector<double> scenario2_v2;
    scenario1_v1.reserve(static_cast<size_t>(rounds));
    scenario1_v2.reserve(static_cast<size_t>(rounds));
    scenario2_v1.reserve(static_cast<size_t>(rounds));
    scenario2_v2.reserve(static_cast<size_t>(rounds));

    int scenario1_v1_pinned = -1;
    int scenario1_v2_pinned = -1;
    int scenario2_v1_pinned = -1;
    int scenario2_v2_pinned = -1;

    std::cout << "\n=== paired rounds: " << rounds
              << ", order=" << order_name(order) << " ===\n";

    for (int round = 0; round < rounds; ++round) {
        const bool v1_first = v1_goes_first(order, round);

        RunResult scenario1_v1_run;
        RunResult scenario1_v2_run;
        if (v1_first) {
            scenario1_v1_run = run_scenario1<Value>(Version::V1, scenario1_cpus);
            scenario1_v2_run = run_scenario1<Value>(Version::V2, scenario1_cpus);
        } else {
            scenario1_v2_run = run_scenario1<Value>(Version::V2, scenario1_cpus);
            scenario1_v1_run = run_scenario1<Value>(Version::V1, scenario1_cpus);
        }
        scenario1_v1_pinned =
            pinned_min(scenario1_v1_pinned, scenario1_v1_run.pinned_threads);
        scenario1_v2_pinned =
            pinned_min(scenario1_v2_pinned, scenario1_v2_run.pinned_threads);
        record_pair(
            1, round, v1_first, scenario1_v1_run, scenario1_v2_run,
            scenario1_v1, scenario1_v2);

        RunResult scenario2_v1_run;
        RunResult scenario2_v2_run;
        if (v1_first) {
            scenario2_v1_run = run_scenario2<Value>(Version::V1, scenario2_cpus);
            scenario2_v2_run = run_scenario2<Value>(Version::V2, scenario2_cpus);
        } else {
            scenario2_v2_run = run_scenario2<Value>(Version::V2, scenario2_cpus);
            scenario2_v1_run = run_scenario2<Value>(Version::V1, scenario2_cpus);
        }
        scenario2_v1_pinned =
            pinned_min(scenario2_v1_pinned, scenario2_v1_run.pinned_threads);
        scenario2_v2_pinned =
            pinned_min(scenario2_v2_pinned, scenario2_v2_run.pinned_threads);
        record_pair(
            2, round, v1_first, scenario2_v1_run, scenario2_v2_run,
            scenario2_v1, scenario2_v2);

        if ((round + 1) % 10 == 0 || round + 1 == rounds) {
            std::cerr << "[bench] completed paired round "
                      << (round + 1) << '/' << rounds << "\n";
        }
    }

    print_scenario_summary(
        1, scenario1_v1_pinned, scenario1_v2_pinned,
        scenario1_v1, scenario1_v2);
    print_scenario_summary(
        2, scenario2_v1_pinned, scenario2_v2_pinned,
        scenario2_v1, scenario2_v2);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    int rounds = 5;
    Order order = Order::Alternate;
    AllocationMode allocation_mode = AllocationMode::System;
    bool strict_environment = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rounds" && i + 1 < argc) {
            if (!parse_positive_int(argv[++i], rounds)) {
                std::cerr << "invalid --rounds value\n";
                return 2;
            }
        } else if (arg == "--order" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "alternate") {
                order = Order::Alternate;
            } else if (value == "alternate-v2") {
                order = Order::AlternateV2;
            } else if (value == "v1-first") {
                order = Order::V1First;
            } else if (value == "v2-first") {
                order = Order::V2First;
            } else {
                std::cerr << "invalid --order value\n";
                return 2;
            }
        } else if (arg == "--allocation" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "system") {
                allocation_mode = AllocationMode::System;
            } else if (value == "arena") {
                allocation_mode = AllocationMode::Arena;
            } else {
                std::cerr << "invalid --allocation value\n";
                return 2;
            }
        } else if (arg == "--strict-env") {
            strict_environment = true;
        } else if (arg == "--help") {
            std::cout << "usage: " << argv[0]
                      << " [--rounds N] "
                         "[--order alternate|alternate-v2|v1-first|v2-first]"
                         " [--allocation system|arena]"
                         " [--strict-env]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }

    const CpuEnvironment environment = inspect_cpu_environment();
    print_cpu_environment(environment, kScenario2Threads, strict_environment);
    std::cout << "allocation_mode=" << allocation_mode_name(allocation_mode)
              << "\n";
    if (strict_environment &&
        !cpu_environment_is_suitable(environment, kScenario2Threads)) {
        std::cerr << "strict environment check failed\n";
        return 3;
    }

    const std::vector<int> scenario1_cpus =
        select_cpus(environment.allowed_cpus, kScenario1Threads);
    const std::vector<int> scenario2_cpus =
        select_cpus(environment.allowed_cpus, kScenario2Threads);

    if (allocation_mode == AllocationMode::Arena) {
        return run_benchmark<ArenaValue>(
            rounds, order, scenario1_cpus, scenario2_cpus);
    }
    return run_benchmark<SystemValue>(
        rounds, order, scenario1_cpus, scenario2_cpus);
}
