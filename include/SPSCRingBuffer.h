#pragma once

#include "NextPowerOfTwo.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

template <typename T>
class SPSCRingBuffer {
public:
    explicit SPSCRingBuffer(size_t capacity)
        : buffer_(llengine::nextPowerOfTwoChecked(capacity)),
          mask_(buffer_.size() - 1)
    {
    }

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    // ---- 生产者专用 ----

    bool tryPush(const T& item)
    {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) >= buffer_.size()) {
            return false; // 满
        }
        buffer_[t & mask_].emplace(item);
        // release：发布上面的写入（消费者 acquire 读 tail_ 后可见）
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    bool tryPush(T&& item)
    {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) >= buffer_.size()) {
            return false;
        }
        buffer_[t & mask_].emplace(std::move(item));
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // 批量入队（拷贝版），返回实际写入条数（<= n）。生产者专用。
    size_t tryPushBatch(const T* items, size_t n)
    {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t space = buffer_.size() - (t - head_.load(std::memory_order_acquire));
        const size_t k = std::min(n, space);
        for (size_t i = 0; i < k; ++i) {
            buffer_[(t + i) & mask_].emplace(items[i]);
        }
        tail_.store(t + k, std::memory_order_release);
        return k;
    }

    // 批量入队（移动版）：逐条 move，支持 move-only 类型；成功入队的源元素会变为
    // moved-from 状态（未入队的那部分保持不变）。生产者专用。
    size_t tryPushBatch(T* items, size_t n)
    {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t space = buffer_.size() - (t - head_.load(std::memory_order_acquire));
        const size_t k = std::min(n, space);
        for (size_t i = 0; i < k; ++i) {
            buffer_[(t + i) & mask_].emplace(std::move(items[i]));
        }
        tail_.store(t + k, std::memory_order_release);
        return k;
    }

    // ---- 消费者专用 ----

    bool tryPop(T& out)
    {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) {
            return false; // 空
        }
        out = std::move(buffer_[h & mask_].value());
        buffer_[h & mask_].reset();
        // release：发布槽位已释放（生产者 acquire 读 head_ 后才可复用该槽）
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // 批量出队，返回实际读出条数（<= max）。消费者专用。
    size_t tryPopBatch(T* out, size_t max)
    {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t available = tail_.load(std::memory_order_acquire) - h;
        const size_t k = std::min(max, available);
        for (size_t i = 0; i < k; ++i) {
            out[i] = std::move(buffer_[(h + i) & mask_].value());
            buffer_[(h + i) & mask_].reset();
        }
        head_.store(h + k, std::memory_order_release);
        return k;
    }

    // ---- 诊断（近似，可在任意线程调用）----

    size_t size() const
    {
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t h = head_.load(std::memory_order_acquire);
        return t >= h ? t - h : 0;
    }

    bool empty() const
    {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    bool full() const
    {
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t h = head_.load(std::memory_order_acquire);
        return t - h >= buffer_.size();
    }

    size_t capacity() const { return buffer_.size(); }

private:
    // head_/tail_ 各占缓存行：生产者写 tail_、消费者写 head_，避免 false sharing
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::vector<std::optional<T>> buffer_;
    size_t mask_;
};
