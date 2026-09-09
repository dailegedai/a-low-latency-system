#include "../include/WorkStealingThreadPool.h"

#include <chrono>

namespace {

// 每个 worker 一次从共享入口转移到本地 deque 的最大任务数
constexpr size_t kLocalBatchSize = 16;

} // namespace

WorkStealingThreadPool::WorkStealingThreadPool(size_t num_threads, size_t queue_size,
                                               WorkStealingRejectPolicy policy)
    : incoming_(queue_size),
      policy_(policy),
      num_threads_(num_threads > 0 ? num_threads : 1)
{
    // 先构造各 worker 的私有 deque（WorkStealingDeque 不可移动/复制，用 unique_ptr）
    for (size_t i = 0; i < num_threads_; ++i) {
        deques_.push_back(std::make_unique<WorkStealingDeque<Task>>(queue_size));
    }

    // 再启动 worker 线程（worker 循环通过索引 me 访问自己的 deque）
    for (size_t i = 0; i < num_threads_; ++i) {
        workers_.emplace_back(std::thread([this, i]() { workerLoop(i); }));
    }
}

WorkStealingThreadPool::~WorkStealingThreadPool()
{
    shutdown();
}

void WorkStealingThreadPool::workerLoop(size_t me)
{
    for (;;) {
        // 1. 本地 LIFO：优先处理自己 deque 的任务（缓存局部性）
        {
            Task task;
            if (deques_[me]->pop(task)) {
                executeTask(task);
                continue;
            }
        }

        // 2. 从共享入口补充一批到本地 deque
        if (fillLocalBatch(me)) {
            continue; // 取到了，下一轮循环 pop 本地
        }

        // 3. steal：从其他 worker 的 deque 头部取一个任务（FIFO）
        {
            Task task;
            if (stealOne(task, me)) {
                executeTask(task);
                continue;
            }
        }

        // 4. 全空：检查是否应该退出，否则进入空闲等待
        {
            std::unique_lock<std::mutex> lock(mtx_);
            if (stop_.load(std::memory_order_acquire) && allEmpty()) {
                return; // stop 且所有任务排空
            }
            workers_idle_.fetch_add(1, std::memory_order_relaxed);
            // 等待任务到达（submit push 后若有空闲 worker 则 cv_.notify_one）
            cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_acquire) || !allEmpty();
            });
            workers_idle_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

void WorkStealingThreadPool::executeTask(Task& task)
{
    busy_workers_.fetch_add(1, std::memory_order_relaxed);

    task.execute(); // 异常由 packaged_task 捕获进 future

    busy_workers_.fetch_sub(1, std::memory_order_relaxed);
    completed_tasks_.fetch_add(1, std::memory_order_relaxed);

    // 执行完一个任务，入口可能腾出空间；若有 BLOCK 生产者等待则唤醒
    if (waiting_producers_.load(std::memory_order_relaxed) > 0) {
        not_full_cv_.notify_one();
    }
}

bool WorkStealingThreadPool::fillLocalBatch(size_t me)
{
    bool any = false;
    for (size_t i = 0; i < kLocalBatchSize; ++i) {
        Task t;
        if (!incoming_.pop(t)) {
            break;
        }
        any = true;
        if (!deques_[me]->push(t)) {
            // 本地 deque 已满：直接执行，避免任务丢失
            // （否则 pop 出入口的任务会因 push 失败而静默消失）
            executeTask(t);
            continue;
        }
        // 入口腾出空间，唤醒 BLOCK 生产者
        if (waiting_producers_.load(std::memory_order_relaxed) > 0) {
            not_full_cv_.notify_one();
        }
    }
    return any;
}

bool WorkStealingThreadPool::stealOne(Task& task, size_t me)
{
    // 从自己之后的 worker 开始轮询，避免每次都从 0 开始竞争
    for (size_t step = 1; step < num_threads_; ++step) {
        size_t target = (me + step) % num_threads_;
        if (target == me) {
            continue;
        }
        if (deques_[target]->steal(task)) {
            return true;
        }
    }
    return false;
}

bool WorkStealingThreadPool::allEmpty() const
{
    for (const auto& dq : deques_) {
        if (!dq->empty()) {
            return false;
        }
    }
    return incoming_.empty();
}

void WorkStealingThreadPool::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stop_.load(std::memory_order_acquire)) {
            return;
        }
        stop_.store(true, std::memory_order_release);
    }

    cv_.notify_all();           // 唤醒空闲 worker 让其检查 stop 退出
    not_full_cv_.notify_all();  // 唤醒 BLOCK 生产者放行

    // 等待所有在途 submit 完成，避免成员析构时仍有提交在访问
    while (active_submits_.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }

    for (Worker& w : workers_) {
        w.join();
    }
}

uint64_t WorkStealingThreadPool::getSubmittedTaskCount() const
{
    return submitted_tasks_.load();
}

uint64_t WorkStealingThreadPool::getCompletedTaskCount() const
{
    return completed_tasks_.load();
}

uint64_t WorkStealingThreadPool::getBusyWorkerCount() const
{
    return busy_workers_.load();
}

size_t WorkStealingThreadPool::getQueueSize()
{
    std::lock_guard<std::mutex> lock(mtx_);
    size_t total = 0;
    for (const auto& dq : deques_) {
        total += dq->size();
    }
    return total; // 不含入口内任务（近似诊断值）
}

bool WorkStealingThreadPool::idle() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return busy_workers_.load() == 0 && allEmpty();
}

size_t WorkStealingThreadPool::getThreadCount() const
{
    return workers_.size();
}

bool WorkStealingThreadPool::isStopping() const
{
    return stop_.load();
}
