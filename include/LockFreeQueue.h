#pragma once

#include "NextPowerOfTwo.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Vyukov 有界 MPMC 无锁队列。
// - 固定容量（向上取 2 的幂），CAS 争用 tail_/head_，sequence 计数器解决 ABA。
// - T 需默认构造 + 可移动赋值。
template <typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity)
        : buffer_(llengine::nextPowerOfTwoChecked(capacity)), mask_(buffer_.size() - 1)
    {
        for (size_t i = 0; i < buffer_.size(); ++i) {
            buffer_[i].sequence.store(static_cast<uint64_t>(i), std::memory_order_relaxed);
        }
    }

    bool push(T&& item)
    {
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Cell* cell = &buffer_[pos & mask_];
            uint64_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                // 槽空闲，用 CAS 认领位置 pos
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
                // CAS 失败，pos 已被更新为当前 tail，重试
            } else if (dif < 0) {
                return false; // 队列已满
            } else {
                pos = tail_.load(std::memory_order_relaxed); // 追不上，重新读取
            }
        }
        buffer_[pos & mask_].data = std::move(item);
        buffer_[pos & mask_].sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item)
    {
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell* cell = &buffer_[pos & mask_];
            uint64_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (dif == 0) {
                // 数据就绪，用 CAS 认领位置 pos
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false; // 队列为空
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
        item = std::move(buffer_[pos & mask_].data);
        buffer_[pos & mask_].sequence.store(pos + buffer_.size(), std::memory_order_release);
        return true;
    }

    // 近似判满（tail/head 异步快照，仅供协调使用）
    bool full() const
    {
        size_t t = tail_.load(std::memory_order_acquire);
        size_t h = head_.load(std::memory_order_acquire);
        return static_cast<intptr_t>(t) - static_cast<intptr_t>(h) >=
               static_cast<intptr_t>(buffer_.size());
    }

    // 近似判空（tail/head 异步快照，仅供协调使用）
    bool empty() const
    {
        size_t t = tail_.load(std::memory_order_acquire);
        size_t h = head_.load(std::memory_order_acquire);
        return static_cast<intptr_t>(t) <= static_cast<intptr_t>(h);
    }

private:
    struct Cell {
        std::atomic<uint64_t> sequence;
        T data;
    };

    // 各自独占缓存行，避免 head/tail 上的 false sharing
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<size_t> head_{0};
    std::vector<Cell> buffer_;
    size_t mask_;
};
