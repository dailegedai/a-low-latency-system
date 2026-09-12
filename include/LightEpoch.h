#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>


namespace llengine {

class LightEpoch {
public:
    LightEpoch() = default;
    LightEpoch(const LightEpoch&) = delete;
    LightEpoch& operator=(const LightEpoch&) = delete;

    // thief 临界区入口：阻塞直到本 deque 不处于 quiesce 窗口
    void enter() noexcept
    {
        for (;;) {
            if (!gate_.load(std::memory_order_acquire)) {
                active_.fetch_add(1, std::memory_order_acq_rel);
                if (!gate_.load(std::memory_order_acquire)) {
                    return; // 进入成功
                }
                // gate 恰在登记后关闭：撤销登记，重试
                active_.fetch_sub(1, std::memory_order_acq_rel);
            }
            pause_or_yield();
        }
    }

    void exit() noexcept
    {
        active_.fetch_sub(1, std::memory_order_acq_rel);
    }

    // owner：进入独占搬移窗口（grow 前）。构造关 gate 等所有在途 thief 退出，
    // 析构重开 gate。窗口内无 thief 在读本 deque 的 Array。
    class QuiesceWindow {
    public:
        explicit QuiesceWindow(LightEpoch& e) noexcept : epoch_(e)
        {
            epoch_.gate_.store(true, std::memory_order_release);
            // StoreLoad 屏障：gate 的 release store 与随后 active_ 的 acquire load
            // 作用在不同原子上，release/acquire 本身不阻止 store→load 重排
            // （x86 TSO 下 store 可滞留写缓冲、load 可提前）。缺此屏障时，
            // owner 可读到 active_==0，同时 thief 读到旧 gate_==false 进入临界区，
            // 出现"窗口内仍有 thief"的违例（ASAN 下实测可复现）。
            std::atomic_thread_fence(std::memory_order_seq_cst);
            while (epoch_.active_.load(std::memory_order_acquire) != 0) {
                pause_or_yield();
            }
        }

        ~QuiesceWindow() noexcept
        {
            epoch_.gate_.store(false, std::memory_order_release);
        }

        QuiesceWindow(const QuiesceWindow&) = delete;
        QuiesceWindow& operator=(const QuiesceWindow&) = delete;

    private:
        LightEpoch& epoch_;
    };

    // thief RAII guard：构造 enter 本 deque 的 epoch，析构 exit
    class Guard {
    public:
        explicit Guard(LightEpoch& e) noexcept : epoch_(e) { epoch_.enter(); }
        ~Guard() noexcept { epoch_.exit(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        LightEpoch& epoch_;
    };

private:
    static void pause_or_yield() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }

    std::atomic<bool> gate_{false};
    std::atomic<uint32_t> active_{0};
};

} // namespace llengine
