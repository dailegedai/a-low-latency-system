# Lock-Free Queue 基准：`LockFreeQueue` vs 有锁实现

> 日期：2026-08-22
> 组件：`include/LockFreeQueue.h`（Vyukov 有界 MPMC 无锁队列，CAS + sequence 计数器）
> 测试：`./build/lock_free_queue_benchmark`（Release -O3, GCC 11, C++17）

## 场景

MPSC：N 个生产者各写 100000 个 `int`，单消费者读到全部。队列容量 = 总量（不因满而阻塞）。
报告 median ms（越小越好），`sample()` warmup=1 + repeats=3。

## 结果

| producers | std::queue+mutex | LockedRingBuffer | LockFreeQueue |
|---|---|---|---|
| 1 | 3.2 | 4.5 | **2.4** |
| 2 | 6.4 | 7.0 | 7.8 |
| 4 | 11.5 | 15.6 | 23.0 |
| 8 | **25.6** | 31.9 | 52.4 |

## 结论（诚实记录：无锁不是万能药）

1. **1 生产者（低争用）**：LockFreeQueue 最快（2.4ms），无锁开销 < 锁开销。✓ 符合预期。
2. **2/4 生产者**：三者接近，锁开始追上。
3. **8 生产者（高争用）**：LockFreeQueue **反而最慢**（52.4ms vs std::queue 25.6ms，约 2x 劣势）。**稳定复现，非噪声。**

### 为什么 8 生产者时无锁更慢

- 所有生产者 CAS 同一个 `tail_`。高争用下 CAS 反复失败重试 → `tail_` 缓存行在核间 ping-pong，开销指数级上升。
- 消费者在 `while(got<total)` 中紧自旋反复 `pop`，对 `head_` 的读取加剧缓存压力。
- 反观 `std::queue+mutex`：临界区极短（一次 push），锁等待让 CPU 让出，反而避免了 CAS 风暴。

**核心教训**：
> 无锁数据结构在**低争用 / 单写者**场景赢；在高争用的多生产者场景，CAS 自旋的缓存一致性开销可能超过互斥锁。选型要看生产者的并发度，不能"无锁 = 快"一刀切。

## 对项目的意义

- 本组件作为独立学习交付（Day28 LockedRingBuffer 之后的无锁对照），验证了 Day28 的定位：ThreadPool 当前瓶颈是**单一 `mtx`**，但**直接换无锁 MPMC 并不能自动解决 8 线程退化**——无锁队列自身的 `tail_` 争用会成为新热点。
- 若要真正解决 ThreadPool threads=8 退化，更有效的方向是：
  - **每 worker 独立队列 + Work Stealing**（避免共享 `tail_`）
  - 或分片队列（sharded queues，每片低争用）
  - 或带批量（batch）入队的无锁队列（减少 CAS 次数）

## 复现

```sh
cmake --build build -t lock_free_queue_benchmark lock_free_queue_test
./build/lock_free_queue_test        # 单线程 FIFO + MPMC 4x4 无丢无重
./build/lock_free_queue_benchmark   # MPSC 三路对比
```
