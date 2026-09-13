#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>


namespace llengine {

// 单调递增任务 ID。只有开启追踪的路径会调用，避免默认热路径上的全局原子竞争。
inline uint64_t nextTaskId() noexcept
{
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

inline uint64_t nowNs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct TaskTrace {
    uint64_t id = 0;          // 单调递增 ID
    uint64_t submit_ns = 0;   // 提交时间戳
    uint64_t complete_ns = 0; // 完成时间戳（任务体执行返回后）
};

// 完成回调。会在 worker 线程执行，可能并发调用。
using TaskTraceSink = std::function<void(const TaskTrace&)>;

} // namespace llengine
