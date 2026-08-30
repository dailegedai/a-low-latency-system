#include "benchmark_util.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <vector>


// A. 标准位移算法（当前 WorkStealingDeque 使用的版本）
static size_t nextPowerOf2_shift(size_t n)
{
    if (n == 0) return 1;
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

// B. 硬件指令 clz 算法（GCC/Clang __builtin_clz，单条硬件指令）
static size_t nextPowerOf2_clz(size_t n)
{
    if (n <= 1) return 1; // n-1 == 0 时 clz(0) 是未定义行为，显式短路
#if defined(__GNUC__) || defined(__clang__)
    return size_t{1} << (std::numeric_limits<size_t>::digits -
                         __builtin_clzll(static_cast<unsigned long long>(n - 1)));
#elif defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanReverse64(&index, static_cast<unsigned __int64>(n - 1));
    return size_t{1} << (index + 1);
#else
    return nextPowerOf2_shift(n);
#endif
}

// 正确性校验：两种实现在全量输入（含边界）下结果必须一致
static bool verify_consistency()
{
    // 逐一遍历 1..2^20 与几个边界值
    for (size_t n = 1; n < (1u << 20); ++n) {
        if (nextPowerOf2_shift(n) != nextPowerOf2_clz(n)) {
            std::cout << "MISMATCH at n=" << n << "\n";
            return false;
        }
    }
    const size_t bounds[] = {
        0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 255, 256, 257,
        size_t{1} << 31, (size_t{1} << 31) + 1,
        size_t{1} << 32, (size_t{1} << 32) + 1,
        size_t{1} << 63        // nextPowerOf2(2^63) = 2^63，可表示
    };
    for (size_t b : bounds) {
        if (nextPowerOf2_shift(b) != nextPowerOf2_clz(b)) {
            std::cout << "MISMATCH at bound n=" << b << "\n";
            return false;
        }
    }
    return true;
}


// 基准：给定一组输入，统计两种实现的总耗时（累加防止被优化掉）
template <typename Fn>
static void bench_one(const char* label, Fn&& fn,
                      const std::vector<size_t>& inputs, volatile size_t& sink)
{
    sample(label, [&] {
        size_t acc = 0;
        for (size_t n : inputs) {
            acc += fn(n);
        }
        sink = acc;
    }, /*warmup=*/1, /*repeats=*/7);
}

static void run_scenario(const char* name, std::vector<size_t> inputs)
{
    std::cout << "--- " << name << " (n=" << inputs.size() << ") ---\n";
    volatile size_t sink_a = 0;
    volatile size_t sink_b = 0;
    bench_one("  shift", nextPowerOf2_shift, inputs, sink_a);
    bench_one("  clz  ", nextPowerOf2_clz, inputs, sink_b);
    std::cout << "\n";
}

int main()
{
    std::cout << "========== nextPowerOf2 Benchmark ==========\n\n";

    if (!verify_consistency()) {
        std::cout << "FAIL: implementations disagree\n";
        return 1;
    }
    std::cout << "correctness: shift == clz (1..2^20 + bounds) PASS\n\n";

    // 输入分布：小值（接近 2 的幂）、大值、随机值
    std::mt19937_64 rng(42);

    {
        std::vector<size_t> in;
        for (int i = 0; i < 500000; ++i) in.push_back((i % 1024) + 1);
        run_scenario("small values 1..1024", std::move(in));
    }
    {
        std::vector<size_t> in;
        for (int i = 0; i < 500000; ++i) in.push_back(static_cast<size_t>(i) + 1);
        run_scenario("sequential 1..500000", std::move(in));
    }
    {
        std::vector<size_t> in;
        for (int i = 0; i < 500000; ++i) in.push_back(rng() % 1000000 + 1);
        run_scenario("random 1..1M", std::move(in));
    }
    {
        std::vector<size_t> in;
        for (int i = 0; i < 500000; ++i) in.push_back(rng());
        run_scenario("random full 64-bit", std::move(in));
    }

    return 0;
}
