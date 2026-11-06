# ll_engine

A learning project: a low-latency C++ thread pool.

C++17, GCC 11, pthreads. Designed to explore the full stack of high-performance
concurrency — from a correct thread pool, to cache-friendly data structures,
memory pooling, and lock-free queues — with measurable benchmarks at every step.

## Build

```sh
cmake -S . -B build          # configure (already done)
cmake --build build          # build all targets
cmake --build build -t <target>  # single target
```

Default build type is Release (`-O3`). Compile options: `-Wall -Wextra -Wpedantic`.
The static library `threadpool` is built from `src/ThreadPool.cpp` + `src/Task.cpp`
+ `src/Worker.cpp` + `src/MemoryPool.cpp` + `src/WorkStealingThreadPool.cpp`; most
executables link it.

## Overview

```
┌─────────────────────────────────────────────────────────────┐
│  ThreadPool                                                 │
│    submit() → std::packaged_task + std::future             │
│    RingBuffer<Task> guarded by mtx  ← zero-alloc hot path   │
│    RejectPolicy: BLOCK / DISCARD / THROW                   │
│    metrics: submitted / completed / busy / queue size      │
│    shutdown() wakes both cv and not_full_cv                │
├─────────────────────────────────────────────────────────────┤
│  Components                                                 │
│    RingBuffer<T>          power-of-2 contiguous ring        │
│    LockedRingBuffer<T>    mutex-guarded wrapper             │
│    LockFreeQueue<T>       Vyukov bounded MPMC, CAS+seq     │
│    MemoryPool             object pool, placement new       │
│    Task                   std::function closure wrapper     │
└─────────────────────────────────────────────────────────────┘
```

## Architecture

### `ThreadPool` — the core (`include/ThreadPool.h`, `src/ThreadPool.cpp`)

- **`submit(F&&, Args&&...)`** is a header template. Each call builds a
  `std::make_shared<std::packaged_task<return_type()>>` bound to the callable,
  obtains a `std::future`, then enqueues a `Task` that invokes the packaged task.
  User exceptions are captured by `std::packaged_task` into the shared state and
  re-thrown on `future.get()`; they never escape the worker thread body.
- **无 future 提交（Phase 2）**: `submitVoid(F&&, Args&&...)` 遵循池的 `RejectPolicy`
  但**不建** `packaged_task`/`future`，`trySubmit(...)` 为非阻塞版（满/停止返回
  `false`）。`allocation_benchmark` 实测把每 submit 分配从 6 次降到 0 次（小闭包）。
  无 future 后用户异常不能传播，由 worker 侧包装层捕获并计入
  `getFailedTaskCount()`。两个池都提供。
- **Task queue** is `RingBuffer<Task>` guarded by a single `std::mutex`.
  The ring is preallocated (capacity rounded up to a power of two), so the
  enqueue/dequeue hot path performs no allocation. See
  `docs/performance_diff.md` for why a ring was chosen over `std::queue`.
- **Reject policies** (`RejectPolicy`):
  - `BLOCK` — wait on `not_full_cv` until space is available (also exits when
    `stop` is set, so producers never hang on shutdown);
  - `DISCARD` — drop the task silently; its future becomes
    `std::future_error: broken_promise`;
  - `THROW` — throw `std::runtime_error("task queue is full.")`.
- **`shutdown()`** sets `std::atomic<bool> stop`, notifies both `cv`
  (consumers) and `not_full_cv` (blocked producers), and joins all workers.
  Workers drain the queue before exiting. `submit()` after shutdown throws.
- **Metrics**: `getSubmittedTaskCount()`, `getCompletedTaskCount()`,
  `getBusyWorkerCount()`, `getFailedTaskCount()`, `getQueueSize()`,
  `idle()` (busy == 0 && queue empty), `getThreadCount()`, `isStopping()`.
  Atomic counters are `alignas(kCacheLineSize)`-isolated to avoid false sharing.

### `RingBuffer<T>` (`include/RingBuffer.h`)

Bounded FIFO over `std::vector<std::optional<T>>`. Capacity is rounded up to a
power of two so head/tail wrap with `& (capacity_ - 1)`. Push/pop are O(1) and
zero-allocating. **Not thread-safe by itself** (guarded by the pool's mutex in
`ThreadPool`, or wrapped by `LockedRingBuffer`).

### `LockedRingBuffer<T>` (`include/LockedRingBuffer.h`)

A `std::mutex`-guarded wrapper around `RingBuffer<T>`, providing thread-safe
`push`/`pop`/`empty`/`size`. Standalone reusable component; not used inside
`ThreadPool` (which already serializes access under its own `mtx`).

### `LockFreeQueue<T>` (`include/LockFreeQueue.h`)

Vyukov bounded MPMC lock-free queue: producers/consumers CAS on `tail_`/`head_`
with per-slot sequence counters (ABA-safe). `head_`/`tail_` are `alignas(64)`-
isolated. Same power-of-two capacity rounding. `T` must be default-constructible
and move-assignable. Benchmark findings (including a high-contention regression)
are documented in `docs/lock_free_queue_benchmark.md`.

### `SPSCRingBuffer<T>` (`include/SPSCRingBuffer.h`)

单生产者/单消费者有界**无锁**环：生产者独占 `tail_`、消费者独占 `head_`，因此**不需要
任何 CAS**，只用 release/acquire 配对；单调递增序号 + `& mask` 定位，可存满容量；
`head_`/`tail_` 各占 `alignas(64)` 缓存行。提供 `tryPush`/`tryPop` 与批量接口
（批量 push 有拷贝/移动两版、批量 pop 移动）。
契约：`push` 仅同一生产者线程调用、`pop` 仅同一消费者线程调用（多生产者请用
`LockFreeQueue`）。对标行情分发的 publisher→handler 单写单读路径。
跨核实测（绑核 cpu0↔cpu1）：无积压往返 RTT p50≈201ns、单程 p50≈100ns；flood 吞吐
~100M msg/s。见 `docs/phase3_spsc_and_pipeline.md`。

### `MemoryPool` (`include/MemoryPool.h`, `src/MemoryPool.cpp`)

Thread-safe object pool for `Task`, built on `::operator new` + placement new:
a chain of pre-constructed blocks and a free list. Supports a fixed
(`expandable=false`, returns `nullptr` on exhaustion) or expanding
(`expandable=true`, grows by a block) mode. `release()` calls `Task::reset()`
to drop any stale closure before the task returns to the pool.

### `Task` (`include/Task.h`, `src/Task.cpp`)

Wraps a `std::function<void()>` — the minimal closure carrier on the hot path.
`execute()` runs the function; `reset()` clears it. Also used as the pooled
object inside `MemoryPool`. (曾带单调 id + 提交时间戳，经测量无人消费属纯开销，
Phase 1 已移除，见 `docs/phase1_measurement_and_task_slimming.md`。)

### `TaskTrace` — opt-in 任务诊断 (`include/TaskTrace.h`)

在 Phase 1 的教训之上，以**默认零成本**的方式重新提供"单调 ID + 提交/完成时间戳"：
`setTraceSink()` 传入非空 sink 后，两个池的每个任务完成时回调一次 `TaskTrace{id,
submit_ns, complete_ns}`（含失败任务）；默认不设置则热路径只多一次可预测的 bool 分支，
不生成 ID、不取时钟、`Task` 仍是 32B。sink 在 worker 线程执行、可能并发，需线程安全。
设计/测试/取舍见 `docs/task_diagnostics_redesign.md`。

### `WorkStealingThreadPool` (`include/WorkStealingThreadPool.h`, `src/WorkStealingThreadPool.cpp`)

无锁 MPMC 共享入口 `LockFreeQueue<Task>` + 每 worker 私有 `WorkStealingDeque<Task>`：
`submit()` 正常路径无锁 push 入口，owner worker 批量搬到本地 deque（LIFO），本地空则
偷别人（FIFO），全空才睡 cv。同样提供无 future 的 `submitVoid()` / `trySubmit()`
（Phase 2）。刻意保留与 `ThreadPool` 的非侵入对比——数据并不支持
"work stealing 全面更快"，详见 `docs/work_stealing_threadpool_benchmark.md`。

### `WorkStealingDeque` / `WorkStealingQueue` V2 / `LightEpoch`

- `WorkStealingDeque`（`include/WorkStealingDeque.h`）— 固定容量 Chase-Lev 风格：
  owner 尾 LIFO / thief 头 FIFO，round 计数单调递增防 ABA。
  开发记录见 `docs/work_stealing_deque_devlog.md`。
- `WorkStealingQueue` V2（`include/WorkStealingQueue.h`）— owner 私有 deque + 动态
  grow：扩容时用 `LightEpoch` quiescence 门等所有在途 thief 退出后再搬移，thief 只
  读 top/array。分阶段设计见 `docs/work_stealing_queue_phases.md`。
- `LightEpoch`（`include/LightEpoch.h`）— per-deque 读写锁语义的轻量 quiescence
  门：owner 关 gate 等 active 归零，thief enter/exit 登记，保护 grow 期间的数组搬移。

## Targets

| Category | Targets |
|---|---|
| App | `main` (`app/main.cpp`), `pipeline_demo` (`app/pipeline_demo.cpp`) |
| Tests (ctest 注册，22 个) | `bounded_queue_test`, `reject_policy_test`, `false_sharing_test`, `metrics_test`, `task_test`, `shutdown_test`, `memory_pool_test`, `placement_new_test`, `ring_buffer_test`, `future_test`, `packaged_task_test`, `decltype_test`, `locked_ring_buffer_test`, `lock_free_queue_test`, `cache_layout_probe`, `work_stealing_deque_test`, `work_stealing_queue_test`, `light_epoch_test`, `work_stealing_threadpool_test`, `no_future_submit_test`, `spsc_ring_buffer_test`, `task_trace_test` |
| Benchmarks | `benchmark`, `submit_benchmark`, `throughput_benchmark`, `latency_benchmark`, `function_benchmark`, `lambda_benchmark`, `queue_benchmark`, `ring_buffer_mpsc_benchmark`, `lock_free_queue_benchmark`, `memory_pool_benchmark`, `false_sharing_benchmark`, `nextpowerof2_benchmark`, `work_stealing_queue_benchmark`, `work_stealing_threadpool_benchmark`, `allocation_benchmark`, `spsc_latency_benchmark` |

> 目标注册表在 `CMakeLists.txt`：链接库的测试/基准进 `THREADPOOL_TARGETS`；注册进
> ctest 的测试进 `LLENGINE_TEST_TARGETS`（改列表时两处同步，勿只改其一）。

## Running / Verifying

Tests/benchmarks 是独立 `main()` 可执行文件 — no test framework. 验证走 ctest：

```sh
ctest --test-dir build --output-on-failure   # 22 个测试一键全量
./scripts/run_all.sh                         # build + ctest (+ 可追加 benchmark 冒烟)
./scripts/sanitize.sh asan                   # ASAN+UBSAN 全量（独立 build-asan）
./scripts/sanitize.sh tsan                   # TSAN 并发测试（独立 build-tsan）
./scripts/stress_tests.sh                    # 并发测试反复跑，查偶发失败（REPEAT=N）
```

Tests use `CHECK(cond)` from `tests/check.h` (prints `FAIL: ... (line N)` and
exits non-zero on failure), so they are meaningful in Release builds.

```sh
./build/shutdown_test        # 8 scenarios: normal/idempotent shutdown, drain,
                             #   submit-after-shutdown reject, future, exception
                             #   propagation, metrics, worker count
./build/reject_policy_test   # THROW / BLOCK / BLOCK-no-hang-on-shutdown / DISCARD
./build/metrics_test         # counter accuracy across the task lifecycle
./build/memory_pool_test     # functional, thread-safety, expand, stale-closure reset
./build/placement_new_test   # placement-new lifetime / destructor balance / reuse
./build/benchmark            # end-to-end throughput (100k tasks)
```

## Benchmarking

`benchmark/benchmark_util.h` provides two tools:

- `Benchmark(name)` — RAII timer printing `name: Xms` on scope exit.
- `sample(label, fn, warmup, repeats)` — runs the callable `warmup` times, then
  `repeats` timed samples, reporting `avg / min / max / median` (ms).

**Use median for conclusions.** See `docs/benchmark_methodology.md` for the
measurement discipline (warmup, repeats, median over mean, same-session-only
comparisons).

Representative benchmark targets:

| Target | Measures |
|---|---|
| `throughput_benchmark` | tasks/sec vs thread count, queue size, work weight, `future.get()`, BLOCK contention, sustained 2M |
| `latency_benchmark` | submit / end-to-end / CPU-bound latency, queue-full contention |
| `submit_benchmark` | pure submit throughput (1M tasks) |
| `queue_benchmark` | `std::queue` vs `RingBuffer` (realloc cycles, throughput, 128B payload) |
| `ring_buffer_mpsc_benchmark` | MPSC: `std::queue+mutex` vs `LockedRingBuffer` |
| `lock_free_queue_benchmark` | MPSC: 3-way `std::queue+mutex` / `LockedRingBuffer` / `LockFreeQueue` |
| `memory_pool_benchmark` | pool latency/throughput, contended, thread-local, Reject vs Expand |
| `false_sharing_benchmark` | micro adjacent-vs-padded counters; ThreadPool threads=1/2/4/8 |
| `allocation_benchmark` | 全局 new/delete 计数：每 submit 分配次数（future vs `submitVoid`），噪声免疫 |
| `spsc_latency_benchmark` | 跨核 SPSC：ping-pong 往返延迟（在途 1 条）+ flood 吞吐；绑核自适应 |
| `pipeline_demo`（app） | 多 producer → `ThreadPool` → 聚合：`saturated` vs `paced` 端到端尾延迟（量化排队延迟） |

## Performance findings (docs)

- `docs/performance_diff.md` — `std::queue<Task>` → `RingBuffer<Task>` in the
  submit path: submit 1M ~1.76x, end-to-end ~1.81x, sustained 2M ~1.71x.
- `docs/false_sharing_benchmark.md` — micro false-sharing costs up to ~26x; but
  `alignas(64)` on ThreadPool counters gives only ±5% (bottleneck is the single
  `mtx`, not the counters).
- `docs/lock_free_queue_benchmark.md` — lock-free wins at low contention (1p),
  but is ~2x slower than a mutex queue at 8 producers (CAS contention on `tail_`).
- `docs/placement_new_benchmark.md` — `MemoryPool` placement-new vs raw
  `new`/`delete` (hot path ~20ns, balanced destructor counts).
- `docs/phase3_spsc_and_pipeline.md` — SPSC 跨核：无积压 RTT p50≈230ns / 单程≈115ns、
  flood ~50-100M msg/s；长尾归因（排除 guest 调度，定位平台层）；pipeline 同一系统
  saturated p50≈15.9ms vs paced p50≈50µs（排队延迟 ~314x）。
- `docs/benchmark_methodology.md` — measurement discipline.

## Learning path (day log)

| Days | Topic | Deliverables |
|---|---|---|
| 26 | MemoryPool thread safety | `MemoryPool` + mutex/free-list, `memory_pool_test` |
| 27 | placement new / object lifetime | pre-constructed blocks, `placement_new_test` |
| 28 | allocator/arena study + expandable pool | block chain, Reject vs Expand, `LockedRingBuffer` |
| 29 | cache-friendly queue + integration | `RingBuffer<Task>` in `ThreadPool`, `performance_diff.md` |
| 30 | false sharing | `alignas(64)` counters, `false_sharing_benchmark` |
| 31 | benchmark methodology | `sample()`/Stats, warmup+median, `benchmark_methodology.md` |
| 32 | correctness hardening | fix BLOCK-hang, stop race, stale closures; CHECK-based tests |

## Week 6+ — Work stealing (implemented)

Day 30/32 measurements identified the single global `mtx` as the real bottleneck
for threads=4/8 scaling (not allocation, not false sharing)。后续按此实现了
work-stealing 路线：

- **`WorkStealingThreadPool`**：无锁 MPMC 入口 + 每 worker 私有
  `WorkStealingDeque`，`submit()` 正常路径去锁。
- **正确性验证**：19 个测试注册进 ctest；`scripts/sanitize.sh` 提供
  ASAN+UBSAN 全量与 TSAN 并发测试（独立 build 目录，GCC TSAN 需 `setarch -R`
  绕过高熵 ASLR，脚本已内置）。
- **诚实结论**（`docs/work_stealing_threadpool_benchmark.md`）：空任务低线程
  快 1.9~2.2x，但 4 线程反被单锁反超——work stealing 是为**不均匀负载**设计，
  均匀快速任务下 steal 是纯开销；因此 README 不再声称"WST 优于 ThreadPool"。
  曾经的 `allocator_plan.md`（池化 `packaged_task`）提议未落地，若日后追进应
  作为独立学习组件，而非性能修复。

## Code style

- `#pragma once`, C++17, `-Wall -Wextra -Wpedantic`, zero-warning build.
- Includes use relative paths from the source file
  (e.g. `#include "../include/ThreadPool.h"`).
- No exceptions in the worker thread body (user exceptions are captured by
  `packaged_task` into the future).
- Tests use `CHECK` from `tests/check.h`; never `assert()`, which is a no-op in
  Release.
- CI / pre-commit / lint / typecheck: none；验证闭环 = ctest + `scripts/sanitize.sh`。
