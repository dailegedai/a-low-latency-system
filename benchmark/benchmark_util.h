#pragma once

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <string>
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
};

// 预热 warmup 次，正式采样 repeats 次；fn 是"一次完整被测工作单元"
template <typename Fn>
static Stats sample(const char* label, Fn&& fn, int warmup = 1, int repeats = 5)
{
    using Clock = std::chrono::steady_clock;

    for (int i = 0; i < warmup; ++i) fn();

    std::vector<double> times;
    times.reserve(repeats);
    for (int i = 0; i < repeats; ++i) {
        auto t0 = Clock::now();
        fn();
        auto t1 = Clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    std::sort(times.begin(), times.end());
    double sum = std::accumulate(times.begin(), times.end(), 0.0);

    Stats s{sum / repeats, times.front(), times.back(), times[times.size() / 2]};
    std::cout << label << ": avg=" << s.avg_ms << "ms"
              << " min=" << s.min_ms << "ms"
              << " max=" << s.max_ms << "ms"
              << " median=" << s.median_ms << "ms\n";
    return s;
}