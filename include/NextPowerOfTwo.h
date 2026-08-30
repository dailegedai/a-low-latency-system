#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>

// =====================================================================
// nextPowerOfTwo 工具：将 size_t 向上取整到 2 的幂。
// 三容器（RingBuffer / LockFreeQueue / WorkStealingDeque）统一复用，
// 消除各自重复的本地实现。
//
// 命名约定（Safe / Checked 成对）：
//   - nextPowerOfTwoSafe   ：可失败，超限返回 std::nullopt；
//   - nextPowerOfTwoChecked：校验后必然成功，超限抛 std::invalid_argument。
// =====================================================================

namespace llengine {

// 将 n 向上取整到 2 的幂；若结果超出 size_t 可表示范围则返回 std::nullopt。
//   - 返回 1        ：n == 0 或 n == 1
//   - 返回 2^k      ：2^(k-1) < n <= 2^k（k >= 1）
//   - 返回 nullopt  ：n > 2^63（结果 >= 2^64，size_t 不可表示）
// 性能：优先硬件指令 clz（GCC/Clang 单条指令），比标准位移算法快约 1.4~2x
//       （实测见 docs/roundup_benchmark.md / benchmark/roundup_benchmark.cpp）。
[[nodiscard]] constexpr std::optional<size_t> nextPowerOfTwoSafe(size_t n)
{
    constexpr size_t digits   = std::numeric_limits<size_t>::digits;  // 64
    constexpr size_t max_pow2 = size_t{1} << (digits - 1);            // 2^63

    if (n <= 1) {
        return size_t{1};        // clz(0) 未定义，显式短路
    }
    if (n > max_pow2) {          // nextPowerOfTwo(n) >= 2^64，不可表示
        return std::nullopt;
    }

    // 编译期常量路径：clz builtin 不是 constexpr，回退到位移算法。
    if (__builtin_is_constant_evaluated()) {
        size_t m = n - 1;
        m |= m >> 1; m |= m >> 2; m |= m >> 4;
        m |= m >> 8; m |= m >> 16; m |= m >> 32;
        return m + 1;
    }

#if defined(__GNUC__) || defined(__clang__)
    return size_t{1} << (digits - __builtin_clzll(static_cast<unsigned long long>(n - 1)));
#elif defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanReverse64(&idx, static_cast<unsigned __int64>(n - 1));
    return size_t{1} << (idx + 1);
#else
    size_t m = n - 1;
    m |= m >> 1; m |= m >> 2; m |= m >> 4;
    m |= m >> 8; m |= m >> 16; m |= m >> 32;
    return m + 1;
#endif
}

// nextPowerOfTwoSafe 的"必然成功"版本：超限（n > 2^63）视为调用方编程错误并抛异常。
// 用于构造函数等"容量必须可表示"的上下文 —— 签名仍返回 size_t，
// 可直接用于成员初始化列表，保持调用点零改动。
[[nodiscard]] inline size_t nextPowerOfTwoChecked(size_t n)
{
    auto r = nextPowerOfTwoSafe(n);
    if (!r) {
        throw std::invalid_argument(
            "nextPowerOfTwo: value exceeds representable range (2^63)");
    }
    return *r;
}

} // namespace llengine
