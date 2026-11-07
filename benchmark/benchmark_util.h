#pragma once

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


class Benchmark {
public:
    Benchmark(std::string name) : name_(name)
    {
        start_ = std::chrono::steady_clock::now();
    }

    ~Benchmark()
    {
        auto end = std::chrono::steady_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_);

        std::cout << name_ << ": " << duration.count() << "ms" << std::endl;
    }

    long elapsedMs() const
    {
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();
    }

private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};


struct Stats {
    double avg_ms = 0;
    double min_ms = 0;
    double max_ms = 0;
    double median_ms = 0;
    double p90_ms = 0;
    double p99_ms = 0;
    bool tail_reliable = false;
};

static Stats summarize_times(std::vector<double> times)
{
    if (times.empty()) {
        throw std::invalid_argument("benchmark requires at least one sample");
    }

    std::sort(times.begin(), times.end());
    double sum = std::accumulate(times.begin(), times.end(), 0.0);

    auto pct = [&](double p) -> double {
        const double idx = p * static_cast<double>(times.size() - 1);
        const size_t lo = static_cast<size_t>(idx);
        const size_t hi = std::min(lo + 1, times.size() - 1);
        const double frac = idx - static_cast<double>(lo);
        return times[lo] + frac * (times[hi] - times[lo]);
    };

    Stats s;
    s.avg_ms = sum / static_cast<double>(times.size());
    s.min_ms = times.front();
    s.max_ms = times.back();
    s.median_ms = pct(0.5);
    s.p90_ms = pct(0.9);
    s.p99_ms = pct(0.99);
    s.tail_reliable = times.size() >= 20;
    return s;
}

static void print_stats(const char* label, const Stats& s, size_t repeats)
{
    std::cout << label << ": avg=" << s.avg_ms << "ms"
              << " min=" << s.min_ms << "ms"
              << " max=" << s.max_ms << "ms"
              << " median=" << s.median_ms << "ms"
              << " p90=" << s.p90_ms << "ms"
              << " p99=" << s.p99_ms << "ms";
    if (!s.tail_reliable) {
        std::cout << " tail_warning=insufficient_samples(n=" << repeats << "<20)";
    }
    std::cout << "\n";
}

// 预热 warmup 次，正式采样 repeats 次；fn 是"一次完整被测工作单元"。
template <typename Fn>
static Stats sample(const char* label, Fn&& fn, int warmup = 1, int repeats = 5)
{
    using Clock = std::chrono::steady_clock;
    if (warmup < 0 || repeats <= 0) {
        throw std::invalid_argument("sample: warmup>=0 and repeats>0 required");
    }

    for (int i = 0; i < warmup; ++i) fn();

    std::vector<double> times;
    times.reserve(static_cast<size_t>(repeats));
    for (int i = 0; i < repeats; ++i) {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    const Stats s = summarize_times(std::move(times));
    print_stats(label, s, static_cast<size_t>(repeats));
    return s;
}

// setup() 在计时区外执行；只对 fn(state) 计时，适合隔离池构造/扩容等准备成本。
template <typename Setup, typename Fn>
static Stats sample_with_setup(
    const char* label,
    Setup&& setup,
    Fn&& fn,
    int warmup = 1,
    int repeats = 5)
{
    using Clock = std::chrono::steady_clock;
    if (warmup < 0 || repeats <= 0) {
        throw std::invalid_argument(
            "sample_with_setup: warmup>=0 and repeats>0 required");
    }

    std::vector<double> times;
    times.reserve(static_cast<size_t>(repeats));
    for (int i = 0; i < warmup + repeats; ++i) {
        auto state = setup();
        const auto t0 = Clock::now();
        fn(state);
        const auto t1 = Clock::now();
        if (i >= warmup) {
            times.push_back(
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
    }

    const Stats s = summarize_times(std::move(times));
    print_stats(label, s, static_cast<size_t>(repeats));
    return s;
}
