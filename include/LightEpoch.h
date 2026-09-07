#pragma once

#include <atomic>
#include <thread>

// =====================================================================
// LightEpoch —— 轻量 quiescence gate（Phase 2）。
//
// 目的：为 work-stealing deque 的 grow() 提供"独占搬移窗口"。
//   owner 需把旧 Array 的数据搬移到新 Array 后才能安全回收旧 Array，
//   前提是"此刻没有 thief 正在读旧 Array"。本原语保证：
//     - thief 在 enter()/exit() 之间处于"活跃"状态；
//     - owner 的 QuiesceWindow 构造时关闭 gate 并等待所有活跃 thief
//       退出 —— 返回后无任何 thief 在读任何 Array；
//     - 窗口销毁（析构）时重开 gate，thief 恢复进入。
//
// 为何不用"世代式 EBR"（文档草案）：
//   世代式（global epoch 轮转 + 延迟两代回收）适合"对象退役后延迟释放、
//   读者永不阻塞"的场景；但 grow 需要的是"顺序搬移数据后发布新数组"，
//   搬移期间不能有并发读者。gate 方案把 grow 变成短暂的独占窗口
//   （thief 仅在该窗口内自旋，grow 罕见故开销可忽略），语义与
//   "grow 需等所有 thief quiescent" 的选择直接对应，实现更简单可验证。
//
// 正确性论证：
//   - enter：先查 gate(false) → active++ → 再查 gate；若第二次发现已关闭
//     则撤销登记并重试。窗口"第一次查(false) 与 active++ 之间被关闭"由
//     双检兜底：active++ 已发生，owner 的 acquire 读 active 会看到并等待，
//     thief 随后撤销到 0，owner 判定 idle——此时 thief 尚未进入临界区
//     （在自旋重试），故不读数据，安全。
//   - QuiesceWindow 构造：gate=true（release）→ 自旋等 active==0（acquire）。
//     已进入的 thief 必然 exit 到 0；未进入的因 gate 关闭而无法进入。
//   - 无死锁：owner 窗口极短（copy 指针 O(n)）；thief 仅短暂自旋。
// =====================================================================

namespace llengine {

class LightEpoch {
public:
    LightEpoch() = delete;

    // thief 临界区入口：阻塞直到不处于 quiesce 窗口
    static void enter() noexcept
    {
        for (;;) {
            if (!s_gate_.load(std::memory_order_acquire)) {
                s_active_.fetch_add(1, std::memory_order_acq_rel);
                if (!s_gate_.load(std::memory_order_acquire)) {
                    return; // 进入成功
                }
                // gate 恰在登记后关闭：撤销登记，重试
                s_active_.fetch_sub(1, std::memory_order_acq_rel);
            }
            pause_or_yield();
        }
    }

    static void exit() noexcept
    {
        s_active_.fetch_sub(1, std::memory_order_acq_rel);
    }

    // owner：进入独占窗口（grow 前）。构造即等所有在途 thief 退出，
    // 析构时重开 gate。窗口内无任何 thief 在读 Array。
    class QuiesceWindow {
    public:
        QuiesceWindow() noexcept
        {
            s_gate_.store(true, std::memory_order_release);
            while (s_active_.load(std::memory_order_acquire) != 0) {
                pause_or_yield();
            }
        }

        ~QuiesceWindow() noexcept
        {
            s_gate_.store(false, std::memory_order_release);
        }

        QuiesceWindow(const QuiesceWindow&) = delete;
        QuiesceWindow& operator=(const QuiesceWindow&) = delete;
    };

    // thief RAII guard：构造 enter，析构 exit
    class Guard {
    public:
        Guard() noexcept { enter(); }
        ~Guard() noexcept { exit(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
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

    inline static std::atomic<bool> s_gate_{false};
    inline static std::atomic<uint32_t> s_active_{0};
};

} // namespace llengine
