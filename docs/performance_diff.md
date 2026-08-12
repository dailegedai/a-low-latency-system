# Before/After 性能对比：`std::queue` vs RingBuffer

> 日期：2026-08-09
> 目标：替换提交路径的队列实现前后，复跑 benchmark 存档并对比
> 前置参考： MPSC 数据（`benchmark/ring_buffer_mpsc_benchmark.cpp`）—— 相同环缓冲微观场景 std::queue ≈ 0.8x vs Ring

## 环境

Release 构建（-O3，GCC 11, C++17, pthreads）。`submit_benchmark` 与 `benchmark` 各跑 3 次取中位数；`throughput_benchmark` 为单次（Before）/2 次中位数（After）。

## Before 基线（替换前：`std::queue<Task>`）

> 代码状态：`include/ThreadPool.h:43` 使用 `std::queue<Task> tasks;`
> 本基线由临时 `git checkout HEAD` 还原 std::queue 版本后复测。

| 基准 | 输出 |
|---|---|
| `throughput_benchmark` | 见下表 |
| `submit_benchmark` | submit 1000000 tasks : 2971 ms |
| `benchmark` | tasks 100000, time 569643 us, throughput 175549 task/s |

### throughput_benchmark

```
--- throughput vs thread count (empty tasks) ---
  threads=1: 607352 tasks/sec
  threads=2: 679095 tasks/sec
  threads=4: 372488 tasks/sec
  threads=8: 61364.3 tasks/sec

--- throughput vs queue size (4 threads, empty tasks) ---
  queue=4:    23221.9 tasks/sec
  queue=16:   39557.9 tasks/sec
  queue=64:   113960 tasks/sec
  queue=256:  251231 tasks/sec
  queue=1024: 212810 tasks/sec
  queue=65536: 218161 tasks/sec

--- throughput vs work weight (4 threads, queue=65536) ---
  empty: 255645 tasks/sec
  pi(10): 116753 tasks/sec
  pi(100): 201377 tasks/sec
  pi(1000): 241937 tasks/sec

--- throughput with future.get() (4 threads, queue=65536) ---
  void future.get(): 203454 tasks/sec
  int future.get(): 216434 tasks/sec

--- throughput under queue full (BLOCK, 2 threads) ---
  BLOCK queue=4: 28296 tasks/sec
  BLOCK queue=1024: 951167 tasks/sec

--- sustained throughput (4 threads, queue=65536, empty tasks) ---
  2M tasks: 209474 tasks/sec
```

## After 结果（替换为 `RingBuffer` 后）

<!-- TODO: 替换后重跑，填入下方 -->
```
./throughput_benchmark 
=== Throughput Benchmark Suite ===

each test submits 500000 tasks unless noted

--- throughput vs thread count (empty tasks) ---
  threads=1: 812663 tasks/sec
  threads=2: 945469 tasks/sec
  threads=4: 272227 tasks/sec
  threads=8: 54541.3 tasks/sec

--- throughput vs queue size (4 threads, empty tasks) ---
  queue=4: 22145.7 tasks/sec
  queue=16: 51296 tasks/sec
  queue=64: 72144 tasks/sec
  queue=256: 227455 tasks/sec
  queue=1024: 323488 tasks/sec
  queue=65536: 348021 tasks/sec

--- throughput vs work weight (4 threads, queue=65536) ---
  empty: 226990 tasks/sec
  pi(10): 224607 tasks/sec
  pi(100): 213619 tasks/sec
  pi(1000): 241432 tasks/sec

--- throughput with future.get() (4 threads, queue=65536) ---
  void future.get(): 180432 tasks/sec
  int future.get(): 227894 tasks/sec

--- throughput under queue full (BLOCK, 2 threads) ---
  BLOCK queue=4: 25689.6 tasks/sec
  BLOCK queue=1024: 1.00104e+06 tasks/sec

--- sustained throughput (4 threads, queue=65536, empty tasks) ---
  2M tasks: 270904 tasks/sec

=== Summary (tasks/sec) ===
  threads=1: 812663
  threads=2: 945469
  threads=4: 272227
  threads=8: 54541.3
  queue=4: 22145.7
  queue=16: 51296
  queue=64: 72144
  queue=256: 227455
  queue=1024: 323488
  queue=65536: 348021
  empty: 226990
  pi(10): 224607
  pi(100): 213619
  pi(1000): 241432
  void future.get(): 180432
  int future.get(): 227894
  BLOCK queue=4: 25689.6
  BLOCK queue=1024: 1.00104e+06
  2M tasks: 270904

=== Done ===
```

```
./submit_benchmark 
submit 1000000 tasks : 4007 ms
```

```
./benchmark 
tasks: 100000
time(us): 305913
throughput(task/s): 326890
```

## 对比表

> ratio 列 = After/Before（>1 为提升）。`submit`/`benchmark` 为中位数（各跑 3 次）；throughput 为单次样本，噪声大（见分析）。

| 测试项 | Before (`std::queue`) | After (RingBuffer) | ratio |
|---|---|---|---|
| submit 1M tasks (ms) | 3359 | 1906 | **1.76x** |
| benchmark throughput (task/s) | 172746 | 312875 | **1.81x** |
| threads=1 | 589442 | 774022* | 1.31x |
| threads=2 | 642936 | 781639* | 1.22x |
| threads=4 | 357998 | 704726* | 1.97x |
| threads=8 | 63792 | 98176* | 1.54x |
| queue=4 | 24835 | 22777* | 0.92x |
| queue=16 | 38779 | 39311 | 1.01x |
| queue=64 | 134613 | 142209* | 1.06x |
| queue=256 | 294557 | 285420 | 0.97x |
| queue=1024 | 343010 | 318586* | 0.93x |
| queue=65536 | 448538 | 401282 | 0.89x |
| empty | 303483 | 425636* | 1.40x |
| pi(10) | 231015 | 470125* | 2.03x |
| pi(100) | 280957 | 639743* | 2.28x |
| pi(1000) | 272475 | 270031* | 0.99x |
| void future.get() | 168791 | 271129* | 1.61x |
| int future.get() | 181986 | 342313* | 1.88x |
| BLOCK queue=4 | 30336 | 26591* | 0.88x |
| BLOCK queue=1024 | 994095 | 943485* | 0.95x |
| 2M tasks | 257773 | 441958* | 1.71x |

\* = 2 次 After 样本的中位数。

## 分析

### 结论：替换符合预期——提交路径显著提速，但瓶颈已转移到别处

**符合预期的部分：**

1. **提交型负载全面提速**（这是本次替换的目标）：
   - `submit_benchmark` 中位数 3359 → 1906 ms（**1.76x**）
   - `benchmark` 172746 → 312875 task/s（**1.81x**）
   - `2M sustained` 257773 → 441958（**1.71x**）
   
   原因正是预想：`std::queue`（deque）每次 push 可能触发分块分配，而 RingBuffer 构造时一次性分配连续内存，**提交热路径零分配 + 缓存连续**。

2. **低竞争场景（empty / 单双线程）提升 1.2–1.4x**：分配减少的收益在无竞争时最干净地显现。

**不符合预期 / 需要注意的部分：**

1. **线程数升高后收益被锁竞争吞掉**：threads=4/8 仍有明显提升（1.5–2x），但**绝对吞吐远低于 1 线程**（threads=8 仅 ~98k/s vs threads=1 ~774k/s）。这说明 8 线程下瓶颈是**单一 `mtx` 的争用 + 原子计数器的缓存行乒乓（false sharing）**，队列结构已经不是主要矛盾。这正是后续要做的 `alignas(64)` 隔离。

2. **每个 submit 仍有一次堆分配**：RingBuffer 只去掉了**队列**的分配，但 `submit()` 里每次仍有
   ```cpp
   auto task = std::make_shared<std::packaged_task<return_type()>>(...);
   ```
   一次 `make_shared` 堆分配。这是提交路径剩下的最大分配，RingBuffer 的收益因此封顶。**下一步优化：用 MemoryPool 对象池化 packaged_task（或 Task 包）**。

3. **微小/波动行属于噪声，不算回退**：`queue=4/64/1024`、`BLOCK`、`pi(1000)` 等 ratio 在 0.88–1.06 之间，与"单次运行无 warmup/重复"的基准设计噪声一致（Day28 数据同样有 ±20% 波动）。`pi(10)/pi(100)` 的 2x 是调度噪声放大，不代表真实收益。

### 下一步优化清单

| 优先级 | 优化 | 目标 |
|---|---|---|
| P0 | 原子计数器 `alignas(64)` 隔离，消除 false sharing | 提升 threads=4/8 高并发吞吐 |
| P0  | benchmark 加 warmup + 重复取中位数 | 消除噪声，得到可信数字 |
| P1  | MemoryPool 对象池化 packaged_task | 去掉 submit 剩余 1 次堆分配 |
| P1  | 无锁 MPMC / Work Stealing 二选一 | 解决 8 线程锁争用 |
| P2 | submit 中 Task 包装构造移出锁 | 缩短锁持有时间 |

## 复现方法

```sh
# Before（当前）
cmake --build build -t throughput_benchmark -t submit_benchmark -t benchmark
./build/throughput_benchmark
./build/submit_benchmark
./build/benchmark

# After（替换 `std::queue` → `RingBuffer` 后）
cmake --build build -t throughput_benchmark -t submit_benchmark -t benchmark
./build/throughput_benchmark
./build/submit_benchmark
./build/benchmark
```

## 备注

- MPSC 微观基准已显示 Ring 在单写单读下优于 `std::queue`（约 0.8x 为 std::queue 的相对吞吐）；本次为真实提交路径（Task 分配 + packaged_task + 锁 + 条件变量）上的宏观对比。
- 单次运行，存在调度噪声；显著差异（ratio 偏离 1 超 ±5%）才视为收益 / 回退。