#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// 测试用确定性同步工具（替代 sleep 定序）。
// 目的：
//   1) "等待某状态成立" -> waitUntil（带超时，真 bug 会快速失败而非挂到 ctest 超时）；
//   2) "让任务占住 worker" -> Gate（替代任务体内 sleep 制造占用）。
namespace testutil {

// 自旋等待 pred() 为真；超时返回 false。主线程/控制线程使用。
template <class Pred>
bool waitUntil(Pred pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// 一次性门闩：release() 之前所有 wait() 阻塞（condition_variable，无忙等）。
// 典型用法：任务体开头 started++ 后 gate.wait()，主线程确认状态后再 release()。
class Gate {
public:
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            open_ = true;
        }
        cv_.notify_all();
    }

    void wait()
    {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool open_ = false;
};

// 计数闩：等待 n 个事件（done() 调用 n 次后放行）。超时返回 false。
class CountDown {
public:
    explicit CountDown(int n) : n_(n) {}

    void done()
    {
        std::lock_guard<std::mutex> lock(m_);
        if (--n_ <= 0) {
            cv_.notify_all();
        }
    }

    bool wait(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, timeout, [this] { return n_ <= 0; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    int n_;
};

} // namespace testutil
