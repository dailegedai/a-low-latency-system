#pragma once

#include "LightEpoch.h"
#include "NextPowerOfTwo.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// =====================================================================
// WorkStealingQueue V2 —— 吸收 Rust 权限隔离模型的 work-stealing deque。
//
// 与 V1（WorkStealingDeque）的核心差异：
//   1. 权限分离：拆成 QueueWorker（owner，独占写）与 QueueStealer
//      （thief，并发读），编译期强制"谁可调用什么"。
//   2. bottom 私有化：bottom 是 QueueWorker 的普通 int64_t（非原子），
//      QueueStealer 不持有 bottom —— 消除热路径上对全局 bottom 的
//      发布/读取同步（跨线程共享只保留 top + array）。
//   3. steal 判空改用 round：QueueStealer 不读 bottom，而是通过槽位
//      round == t+1 判断"该位置是否已有数据"，从而不依赖 owner 的
//      bottom 快照。
//
// 存储：元素以 std::atomic<T*> 存放（V1 已验正的方案），push new、
// 消费方 delete、所有权唯一。
// 生命周期：Worker/Stealer 共享 std::shared_ptr<SharedData>；最后持有者
// 析构时释放数组并清理未消费元素。
//
// Phase 1 范围：固定容量（不支持 grow），无 epoch —— 仅验证权限隔离
// 重构的正确性。grow / epoch 见后续 Phase。
// =====================================================================

namespace llengine {

template <typename T>
class QueueShared;   // 内部共享状态

template <typename T>
class QueueStealer;  // thief：只读 top/array

template <typename T>
class QueueWorker {  // owner：独占 push/pop，持私有 bottom
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
    bool push(const T& item);  // 尾插 LIFO；满返回 false（Phase 1 不支持 grow）
    bool pop(T& item);         // 尾取 LIFO

    size_t size() const;       // 近似（top 异步）
    bool empty() const;
    size_t capacity() const;

private:
    friend class QueueStealer<T>;

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
        // 清理未消费元素：被消费的槽 data 已 exchange 置空，剩余非空即需释放。
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
    std::atomic<Array*> array_;                 // 当前数组（Phase 1 固定）
};

// =====================================================================
// QueueWorker —— owner 实现
// =====================================================================

template <typename T>
bool QueueWorker<T>::push(const T& item)
{
    QueueShared<T>* s = shared_.get();
    typename QueueShared<T>::Array* a = s->array_.load(std::memory_order_acquire);

    int64_t b = bottom_;
    int64_t t = s->top.load(std::memory_order_acquire);
    if (b - t >= static_cast<int64_t>(a->mask) + 1) {
        return false; // 满
    }

    typename QueueShared<T>::Cell* cell = &a->cells[static_cast<uint64_t>(b) & a->mask];
    // 槽位环绕复用：上一轮数据必须已被消费方置空；若并发消费者仍在 exchange，
    // 等待其完成（有界），避免覆盖未消费数据。
    while (cell->data.load(std::memory_order_acquire) != nullptr) {
        std::this_thread::yield();
    }
    cell->data.store(new T(item), std::memory_order_release);
    cell->round.store(static_cast<uint64_t>(b) + 1, std::memory_order_release);
    bottom_ = b + 1; // 普通写，owner 私有
    return true;
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
    // 全路径进 guard：Phase 3 grow 会以 QuiesceWindow 等待所有 thief 退出后再
    // 搬移旧 Array；此处 enter/exit 保证本 steal 在窗口内不被搬移打断。
    LightEpoch::Guard epoch_guard;

    QueueShared<T>* s = shared_.get();

    int64_t t = s->top.load(std::memory_order_acquire);
    typename QueueShared<T>::Array* a = s->array_.load(std::memory_order_acquire);
    typename QueueShared<T>::Cell* cell = &a->cells[static_cast<uint64_t>(t) & a->mask];

    // round 判空 + 防 ABA：
    //   位置 t 有数据 ⟺ push(t) 已 release 写 round = t+1 且未被消费。
    //   若 round != t+1（未 push / 已消费 / 旧轮次残留）则视为空，返回 false。
    //   （不再依赖 owner 的 bottom —— 这是去掉 bottom 共享的前提。）
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
    QueueShared<T>* s = shared_.get();
    int64_t t = s->top.load(std::memory_order_acquire);
    typename QueueShared<T>::Array* a = s->array_.load(std::memory_order_acquire);
    typename QueueShared<T>::Cell* cell = &a->cells[static_cast<uint64_t>(t) & a->mask];
    return cell->round.load(std::memory_order_acquire) != static_cast<uint64_t>(t) + 1;
}

} // namespace llengine
