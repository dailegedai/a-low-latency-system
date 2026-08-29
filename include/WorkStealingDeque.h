#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>


template <typename T>
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(size_t capacity)
        : capacity_(roundUp(capacity)),
          mask_(capacity_ - 1),
          cells_(capacity_)
    {
        for (Cell& c : cells_) {
            c.round.store(0, std::memory_order_relaxed);
        }
    }

    ~WorkStealingDeque()
    {
        // 释放仍在队列中的元素（未被消费的部分）
        size_t t = top_.load(std::memory_order_relaxed);
        size_t b = bottom_.load(std::memory_order_relaxed);
        for (size_t i = t; i < b; ++i) {
            delete cells_[i & mask_].data.load(std::memory_order_relaxed);
        }
    }

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;


    // 尾插；仅 owner 调用。队列满时返回 false（调用方需重试或丢弃）。
    bool push(const T& item)
    {
        size_t b = bottom_.load(std::memory_order_relaxed);
        size_t t = top_.load(std::memory_order_acquire);
        if (static_cast<intptr_t>(b) - static_cast<intptr_t>(t) >=
            static_cast<intptr_t>(capacity_)) {
            return false; // 满
        }

        Cell* cell = &cells_[b & mask_];
        // 槽位环绕复用：该物理槽上一轮的 data 必须已被消费方置空（exchange nullptr）。
        // 若并发消费者尚未完成 exchange（残留非空），等待其完成，避免覆盖未消费数据。
        while (cell->data.load(std::memory_order_acquire) != nullptr) {
            std::this_thread::yield();
        }
        cell->data.store(new T(item), std::memory_order_release);
        cell->round.store(static_cast<uint64_t>(b) + 1, std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    // 尾取（LIFO）；仅 owner 调用。返回是否取到。
    bool pop(T& item)
    {
        size_t b = bottom_.load(std::memory_order_relaxed);
        if (b == 0) {
            return false;
        }
        --b;
        bottom_.store(b, std::memory_order_relaxed);

        size_t t = top_.load(std::memory_order_acquire);
        if (static_cast<intptr_t>(b) < static_cast<intptr_t>(t)) {
            bottom_.store(t, std::memory_order_relaxed); // 被 thief 偷空
            return false;
        }

        Cell* cell = &cells_[b & mask_];
        if (static_cast<intptr_t>(b) > static_cast<intptr_t>(t)) {
            // 正常 LIFO 取尾
            T* p = cell->data.exchange(nullptr, std::memory_order_acq_rel);
            if (p == nullptr) {
                return false;
            }
            // 消费后置 round 无效（0）：仅当 round 仍是自己的旧值时才置 0（CAS 保护），
            // 避免覆盖 owner 环绕复用该槽时已写入的新 round。
            uint64_t old_round = static_cast<uint64_t>(b) + 1;
            cell->round.compare_exchange_weak(old_round, 0, std::memory_order_acq_rel,
                                            std::memory_order_relaxed);
            item = std::move(*p);
            delete p;
            return true;
        }

        // b == t：只剩一个元素，owner 与 thief 竞争
        if (!top_.compare_exchange_weak(t, t + 1,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
            bottom_.store(t + 1, std::memory_order_relaxed); // thief 抢走
            return false;
        }
        T* p = cell->data.exchange(nullptr, std::memory_order_acq_rel);
        if (p == nullptr) {
            return false;
        }
        {
            uint64_t old_round = static_cast<uint64_t>(b) + 1;
            cell->round.compare_exchange_weak(old_round, 0, std::memory_order_acq_rel,
                                            std::memory_order_relaxed);
        }
        item = std::move(*p);
        delete p;
        return true;
    }


    // 头取（FIFO）；可被多个 thief 并发调用。返回是否取到。
    bool steal(T& item)
    {
        size_t t = top_.load(std::memory_order_acquire);
        size_t b = bottom_.load(std::memory_order_acquire);
        if (static_cast<intptr_t>(t) >= static_cast<intptr_t>(b)) {
            return false; // 空
        }

        Cell* cell = &cells_[t & mask_];
        // round 比对：push 以 release 写 round = b+1（单调递增），thief acquire 读并
        // 与 t+1 比对，验证该槽是本轮数据、而非环绕复用的旧轮次残留。
        // 单调递增是防 ABA 的根本，比对只是利用单调性的手段。
        if (cell->round.load(std::memory_order_acquire) != static_cast<uint64_t>(t) + 1) {
            return false;
        }
        if (!top_.compare_exchange_weak(t, t + 1,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
            return false; // 竞争失败
        }

        // 认领成功，独占位置 t：取走指针（exchange 原子，所有权唯一）。
        T* p = cell->data.exchange(nullptr, std::memory_order_acq_rel);
        if (p == nullptr) {
            return false;
        }
        // 消费后置 round 无效（0）：仅当 round 仍是自己的旧值时才置 0（CAS 保护），
        // 避免覆盖 owner 环绕复用该槽时已写入的新 round。
        {
            uint64_t old_round = static_cast<uint64_t>(t) + 1;
            cell->round.compare_exchange_weak(old_round, 0, std::memory_order_acq_rel,
                                            std::memory_order_relaxed);
        }
        item = std::move(*p);
        delete p;
        return true;
    }

    size_t capacity() const { return capacity_; }

    // 近似的元素数量（top/bottom 异步，仅供诊断）
    size_t size() const
    {
        size_t t = top_.load(std::memory_order_acquire);
        size_t b = bottom_.load(std::memory_order_acquire);
        return b > t ? b - t : 0;
    }

    bool empty() const { return size() == 0; }

private:
    static size_t roundUp(size_t n)
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

    struct Cell {
        std::atomic<T*> data{nullptr};
        std::atomic<uint64_t> round{0};
    };

    std::atomic<size_t> top_{0};    // thief 读/写
    std::atomic<size_t> bottom_{0}; // owner 写，thief 读
    size_t capacity_;
    size_t mask_;
    std::vector<Cell> cells_;
};
