# False Sharing 基准：`alignas(64)` 前后对比

> 日期：2026-08-12
> 目标：量化"原子计数器 false sharing"的代价，并验证 ThreadPool 上加 `alignas(64)` 的收益
> 测试：`./build/false_sharing_benchmark`（Release -O3, GCC 11, C++17）

## 实验方法

`false_sharing_benchmark` 分两部分：

- **Part 1（Micro）**：N 线程各自 `fetch_add` 自己的计数器，对比"相邻缓存行"（`AdjacentCounter`）与"`alignas(64)` 隔离"（`PaddedCounter`），`ITERS = 100M`。
- **Part 2（Macro）**：真实 `ThreadPool` 500k 个空任务吞吐，threads = 1/2/4/8。

**Before** = 临时移除 `ThreadPool.h` 中 3 个原子计数器的 `alignas(...)`；**After** = 恢复 `alignas`。各跑 3 次取中位数。

## Part 1 — Micro：false sharing 现象（与 ThreadPool 无关，纯计数器）

| threads | adjacent | padded | speedup |
|---|---|---|---|
| 2 | ~1237 ms | ~199 ms | **6.2x** |
| 4 | ~3200 ms | ~191 ms | **16.8x** |
| 8 | ~6500 ms | ~243 ms | **26.9x** |

> 同一份代码，只差 `alignas(64)`，8 线程差 27 倍 —— false sharing 现象的教科书级复现。
> Before/After 两组此数据一致（该部分与 ThreadPool 无耦合，不随 alignas 变化）。

## Part 2 — Macro：ThreadPool 500k 任务（ms，越低越好）

| threads | Before (无 alignas) | After (alignas) | 变化 |
|---|---|---|---|
| 1 | 776 | 741 | -4.5% |
| 2 | 663 | 639 | -3.6% |
| 4 | 804 | 842 | +4.7% |
| 8 | 7924 | 7611 | -4.0% |

原始 3 次数据：
- Before: `t1=857/751/776  t2=723/663/599  t4=992/674/804  t8=7924/6343/8432`
- After : `t1=733/824/741  t2=648/639/612  t4=813/842/1044  t8=9138/6095/7611`

## 结论（与预期不符，诚实记录）

**Micro 现象成功复现（5~27x），但 ThreadPool 上 alignas 几乎无收益（±5% 噪声内）。**

预期"threads=4/8 应有可测提升"**没有实现**。原因：

1. **真正的瓶颈是单一共享互斥锁 `mtx`，不是原子计数器。**
   - 每个 submit / 每个 worker pop 都要 acquire/release `mtx`，锁竞争随线程数急剧上升。
   - 三个计数器（submitted/completed/busy）更新频率远低于锁操作，即使它们 false-share，代价也被锁的串行化掩盖。
2. **Part 2 的 threads=8 绝对性能极差**（~8s vs t1 的 ~0.75s），高线程下 ThreadPool 的可扩展性被锁扼杀。
3. Micro 与 Macro 的差异说明：**微观层 false sharing 存在 ≠ 它在当前架构下是瓶颈**。优化前必须用剖析确认热路径。

## 对本项目的意义 / 下一步

- `alignas(64)` 保留（无害，正确的工程实践，消除潜在隐患），但**不能作为简历上的吞吐提升证据**。
- 真正要解决 threads=8 退化的是 **Week6 候选优化**：
  - 无锁 MPMC / 有界环形队列（去掉单一 `mtx`）
  - 或 Work Stealing
- 若想继续验证"计数器更新路径"，可改用 **MPSC 直接压测原子计数**（去掉锁的影响），而非经 ThreadPool 间接测。

## 复现

```sh
# After（当前）
cmake --build build -t false_sharing_benchmark
./build/false_sharing_benchmark

# Before：临时移除 ThreadPool.h 三个 alignas 后重编译复测
```
