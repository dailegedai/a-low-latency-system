#pragma once

#include "LightEpoch.h"
#include "NextPowerOfTwo.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>


namespace llengine {

template <typename T>
class QueueShared;   // 内部共享状态

template <typename T>
class QueueStealer;  // thief：只读 top/array

template <typename T>
class QueueWorker {  // owner：独占 push/pop/grow，持私有 bottom
public:
    explicit QueueWorker(size_t capacity)
        : shared_(std::make_shared<QueueShared<T>>(capacity)), bottom_(0)
    {
    }

    ~QueueWorker() = default;
    QueueWorker(const QueueWorker&) = delete;
    QueueWorker& operator=(const QueueWorker&) = delete;

    QueueStealer<T> make_stealer() const;  // 返回共享同一 SharedData 的 thief 视图

    // ---- owner 专用 ----
    bool push(const T& item);  // 尾插 LIFO；满时自动 grow（Phase 3），恒返回 true
    bool pop(T& item);         // 尾取 LIFO

    size_t size() const;       // 近似（top 异步）
    bool empty() const;
    size_t capacity() const;   // 当前 Array 容量（grow 后翻倍）

private:
    friend class QueueStealer<T>;

    void grow();               // owner：容量满时扩容

    std::shared_ptr<QueueShared<T>> shared_;
    int64_t bottom_{0};        // 普通有符号：owner 私有，非原子
};

template <typename T>
class QueueStealer {
public:
    explicit QueueStealer(std::shared_ptr<QueueShared<T>> shared)
        : shared_(std::move(shared))
    {
    }

    // ---- thief 专用 ----
    bool steal(T& item);  // 头取 FIFO；round 判空，不读 bottom
    bool empty() const;   // round 判空（近似，仅供诊断）

private:
    std::shared_ptr<QueueShared<T>> shared_;
};

// =====================================================================
// 内部共享状态
// =====================================================================

template <typename T>
class QueueShared {
public:
    explicit QueueShared(size_t capacity)
        : array_(new Array(llengine::nextPowerOfTwoChecked(capacity)))
    {
    }

    ~QueueShared()
    {
        // 清理当前（最新）Array 中未消费元素：被消费/grow 搬移的槽 data 已
        // exchange 置空，剩余非空即需释放。所有未消费数据最终都在最新 Array。
        Array* a = array_.load(std::memory_order_relaxed);
        for (auto& cell : a->cells) {
            T* p = cell.data.exchange(nullptr, std::memory_order_acq_rel);
            delete p;
        }
        delete a;
    }

    QueueShared(const QueueShared&) = delete;
    QueueShared& operator=(const QueueShared&) = delete;

    struct Cell {
        std::atomic<T*> data{nullptr};
        std::atomic<uint64_t> round{0};
    };

    struct Array {
        explicit Array(size_t cap)
            : mask(cap - 1), cells(cap) {}
        const size_t mask;
        std::vector<Cell> cells;
    };

    alignas(64) std::atomic<int64_t> top{0};   // 序号（有符号）；thief CAS / owner 读
    std::atomic<Array*> array_;                 // 当前数组（grow 会替换）
    LightEpoch epoch;                           // per-deque quiescence gate（grow 用）
};


// QueueWorker —— owner 实现
template <typename T>
bool QueueWorker<T>::push(const T& item)
{
    QueueShared<T>* s = shared_.get();

    for (;;) {
        typename QueueShared<T>::Array* a = s->array_.load(std::memory_order_acquire);
        int64_t b = bottom_;
        int64_t t = s->top.load(std::memory_order_acquire);

        if (b - t < static_cast<int64_t>(a->mask) + 1) {
            // 有空间：写入逻辑位置 b 对应的物理槽
            typename QueueShared<T>::Cell* cell = &a->cells[static_cast<uint64_t>(b) & a->mask];
            // 槽位环绕复用：该物理槽上一轮数据必须已被消费方置空（pop/steal
            // exchange 或 grow 搬移时 exchange），正常必空；此处仅防御并发 thief
            // 恰好正在 exchange 的极短窗口（有界等待）。
            while (cell->data.load(std::memory_order_acquire) != nullptr) {
                std::this_thread::yield();
            }
            cell->data.store(new T(item), std::memory_order_release);
            cell->round.store(static_cast<uint64_t>(b) + 1, std::memory_order_release);
            bottom_ = b + 1; // 普通写，owner 私有
            return true;
        }

        // 满：扩容后重试（push 恒成功，除非分配失败抛异常）
        grow();
    }
}

template <typename T>
void QueueWorker<T>::grow()
{
    QueueShared<T>* s = shared_.get();
    typename QueueShared<T>::Array* old_a = s->array_.load(std::memory_order_acquire);
    size_t old_cap = old_a->mask + 1;
    size_t new_cap = old_cap * 2;

    // 1) 先分配新 Array（在 QuiesceWindow 外，避免持有窗口做慢分配）
    typename QueueShared<T>::Array* new_a =
        new typename QueueShared<T>::Array(new_cap);

    // 2) 进入本 deque 的独占搬移窗口：等所有偷本 deque 的 thief 退出
    LightEpoch::QuiesceWindow window(s->epoch);

    // 3) 窗口内 top 稳定（无 thief CAS），bottom 为 owner 私有。
    int64_t t = s->top.load(std::memory_order_acquire);
    int64_t b = bottom_;

    // 4) 把 [t, b) 的指针搬到新 Array，并用 exchange 置空旧槽。
    //    置空是关键：否则旧槽残留"影子指针"，后续 push 环绕写回该物理槽时
    //    会误以为 data 未消费而无限等待（实测死锁根因）。
    //    （窗口内无 thief，exchange 安全；数据所有权转给 new_a。）
    for (int64_t i = t; i < b; ++i) {
        typename QueueShared<T>::Cell* oc =
            &old_a->cells[static_cast<uint64_t>(i) & old_a->mask];
        T* p = oc->data.exchange(nullptr, std::memory_order_acq_rel);
        if (p == nullptr) {
            continue; // 该槽已被消费（窗口建立前 thief 偷走），跳过
        }
        typename QueueShared<T>::Cell* nc =
            &new_a->cells[static_cast<uint64_t>(i) & new_a->mask];
        nc->data.store(p, std::memory_order_relaxed);
        nc->round.store(static_cast<uint64_t>(i) + 1, std::memory_order_release);
    }

    // 5) 发布新 Array（release store 使后续 thief 可见完整数据）
    s->array_.store(new_a, std::memory_order_release);

    // 6) 释放旧 Array 本体：其 [t,b) 槽已 exchange 置空，<t 早空，无残留数据；
    //    不 delete 指向的 T（数据已在 new_a，由其消费方 delete，防 double-free）。
    delete old_a;
}

template <typename T>
bool QueueWorker<T>::pop(T& item)
{
    QueueShared<T>* s = shared_.get();
    typename QueueShared<T>::Array* a = s->array_.load(std::memory_order_acquire);

    int64_t b = bottom_;
    if (b == 0) {
        return false;
    }
    int64_t t = s->top.load(std::memory_order_acquire);

    if (b - 1 > t) {
        // 正常 LIFO 取尾（位置 b-1，无 thief 竞争：thief 只从 top 侧）
        typename QueueShared<T>::Cell* cell = &a->cells[static_cast<uint64_t>(b - 1) & a->mask];
        T* p = cell->data.exchange(nullptr, std::memory_order_acq_rel);
        if (p == nullptr) {
            return false; // 防御（owner 独占 b-1 > t，正常不触发）
        }
        // 消费后 CAS 置 round 无效（0）：防环绕复用后 thief 误读"旧 round + 空 data"；
        // 仅当 round 仍是自己的（= b）才清，避免覆盖 owner 环绕后已写入的新 round。
        uint64_t expected_round = static_cast<uint64_t>(b);
        cell->round.compare_exchange_weak(expected_round, 0, std::memory_order_acq_rel,
                                          std::memory_order_relaxed);
        bottom_ = b - 1;
        item = std::move(*p);
        delete p;
        return true;
    }

    if (b - 1 < t) {
        return false; // 已被 thief 偷空
    }

    // b - 1 == t：只剩一个元素，owner 与 thief 竞争
    int64_t old = t;
    if (!s->top.compare_exchange_weak(old, t + 1, std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
        return false; // thief 抢走
    }
    typename QueueShared<T>::Cell* cell = &a->cells[static_cast<uint64_t>(t) & a->mask];
    T* p = cell->data.exchange(nullptr, std::memory_order_acq_rel);
    bottom_ = t + 1;
    if (p == nullptr) {
        return false;
    }
    {
        uint64_t expected_round = static_cast<uint64_t>(t) + 1;
        cell->round.compare_exchange_weak(expected_round, 0, std::memory_order_acq_rel,
                                          std::memory_order_relaxed);
    }
    item = std::move(*p);
    delete p;
    return true;
}

template <typename T>
QueueStealer<T> QueueWorker<T>::make_stealer() const
{
    return QueueStealer<T>(shared_);
}

template <typename T>
size_t QueueWorker<T>::size() const
{
    QueueShared<T>* s = shared_.get();
    int64_t b = bottom_;
    int64_t t = s->top.load(std::memory_order_acquire);
    return b > t ? static_cast<size_t>(b - t) : 0;
}

template <typename T>
bool QueueWorker<T>::empty() const
{
    return size() == 0;
}

template <typename T>
size_t QueueWorker<T>::capacity() const
{
    QueueShared<T>* s = shared_.get();
    typename QueueShared<T>::Array* a = s->array_.load(std::memory_order_acquire);
    return a->mask + 1;
}

// =====================================================================
// QueueStealer —— thief 实现（不读 bottom，round 判空）
// =====================================================================

template <typename T>
bool QueueStealer<T>::steal(T& item)
{
    // 进入目标 deque 的 epoch（per-deque gate）：grow 会以 QuiesceWindow 等本
    // deque 的所有 thief 退出后再搬移旧 Array；此处保证本 steal 不被搬移打断。
    LightEpoch::Guard epoch_guard(shared_->epoch);

    QueueShared<T>* s = shared_.get();

    int64_t t = s->top.load(std::memory_order_acquire);
    typename QueueShared<T>::Array* a = s->array_.load(std::memory_order_acquire);
    typename QueueShared<T>::Cell* cell = &a->cells[static_cast<uint64_t>(t) & a->mask];

    // round 判空 + 防 ABA：
    //   位置 t 有数据 ⟺ push(t) 已 release 写 round = t+1 且未被消费。
    //   若 round != t+1（未 push / 已消费 / 旧轮次残留）则视为空，返回 false。
    if (cell->round.load(std::memory_order_acquire) != static_cast<uint64_t>(t) + 1) {
        return false;
    }
    if (!s->top.compare_exchange_weak(t, t + 1, std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
        return false; // 竞争失败
    }

    // 认领成功，独占位置 t
    T* p = cell->data.exchange(nullptr, std::memory_order_acq_rel);
    if (p == nullptr) {
        return false; // 防御（round==t+1 应保证 data 可见且非空）
    }
    // 消费后 CAS 置 round 无效（0），防环绕复用误判
    {
        uint64_t expected_round = static_cast<uint64_t>(t) + 1;
        cell->round.compare_exchange_weak(expected_round, 0, std::memory_order_acq_rel,
                                          std::memory_order_relaxed);
    }
    item = std::move(*p);
    delete p;
    return true;
}

template <typename T>
bool QueueStealer<T>::empty() const
{
    LightEpoch::Guard epoch_guard(shared_->epoch); // 读 array/round 需防 grow 释放
    QueueShared<T>* s = shared_.get();
    int64_t t = s->top.load(std::memory_order_acquire);
    typename QueueShared<T>::Array* a = s->array_.load(std::memory_order_acquire);
    typename QueueShared<T>::Cell* cell = &a->cells[static_cast<uint64_t>(t) & a->mask];
    return cell->round.load(std::memory_order_acquire) != static_cast<uint64_t>(t) + 1;
}

} // namespace llengine
